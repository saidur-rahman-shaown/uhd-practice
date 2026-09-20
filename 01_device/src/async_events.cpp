#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

/*
 * A guided tour of the status characters UHD prints while streaming.
 *
 * While a transfer is running UHD writes a single character straight to
 * stderr for each problem it notices, with no newline and no context:
 *
 *   U  underflow      the transmitter ran out of samples mid-burst
 *   L  late packet    a burst arrived after its scheduled time had passed
 *   S  sequence error the device rejected a packet as out of order
 *   O  overflow       the receiver produced samples faster than we read them
 *   D  dropped        a packet was lost on the way to the host
 *
 * They appear inline, so output looks like "LLLLUS" jammed against whatever
 * else is being printed. They are easy to miss and easy to misread.
 *
 * Seeing none of them is the normal, healthy case -- which makes them awkward
 * to learn from, because you only meet them by accident when something breaks.
 * Each mode here provokes one deliberately, then reads the matching async
 * message back from the device so the character and its meaning appear
 * together.
 */


void legend()
{
    std::cout
        << "\nWatch stderr while each test runs. UHD prints one character\n"
        << "per event, inline and without newlines:\n\n"
        << "    U  underflow      L  late packet    S  sequence error\n"
        << "    O  overflow       D  dropped packet\n\n"
        << "The decoded async messages are printed underneath.\n";
}


// ============================================================
// A clean burst: what success looks like
// ============================================================

void demo_ack(uhd::usrp::multi_usrp::sptr usrp, const Config& cfg)
{
    std::cout
        << "\n====================================================\n"
        << "1. CLEAN BURST  (expect: BURST_ACK, no characters)\n"
        << "====================================================\n"
        << "Scheduling 10 bursts 20 ms into the future -- plenty of\n"
        << "warning for the device. Nothing should be printed to\n"
        << "stderr at all.\n\n";

    auto tx = usrp->get_tx_stream(uhd::stream_args_t("fc32", otw_format()));
    const auto samples = make_waveform(2000, cfg.sample_rate, 10e3);

    size_t acks = 0, others = 0;

    for (size_t i = 0; i < 10; ++i)
    {
        uhd::tx_metadata_t md;
        md.start_of_burst = true;
        md.end_of_burst   = true;
        md.has_time_spec  = true;
        md.time_spec      = usrp->get_time_now() + 0.020;

        tx->send(samples.data(), samples.size(), md, 1.0);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        uhd::async_metadata_t am;
        while (tx->recv_async_msg(am, 0.05))
        {
            if (am.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK)
                ++acks;
            else
            {
                ++others;
                std::cout << "  unexpected: "
                          << async_event_to_string(am.event_code) << "\n";
            }
        }
    }

    std::cout
        << "  BURST_ACK   = " << acks << " of 10\n"
        << "  anything else = " << others << "\n"
        << "\nThis is the baseline. Every burst landed on time, so UHD\n"
        << "had nothing to report.\n";
}


// ============================================================
// Late packets: the L
// ============================================================

void demo_late(uhd::usrp::multi_usrp::sptr usrp, const Config& cfg)
{
    std::cout
        << "\n====================================================\n"
        << "2. LATE PACKETS  (expect: L, and TIME_ERROR)\n"
        << "====================================================\n"
        << "Scheduling 10 bursts 10 ms in the PAST. The device cannot\n"
        << "transmit into the past, so it drops each one and reports\n"
        << "TIME_ERROR. Note that send() still succeeds -- the host\n"
        << "handed over the samples, which is all send() promises.\n\n";

    auto tx = usrp->get_tx_stream(uhd::stream_args_t("fc32", otw_format()));
    const auto samples = make_waveform(2000, cfg.sample_rate, 10e3);

    size_t late = 0, acks = 0, sent_ok = 0;

    for (size_t i = 0; i < 10; ++i)
    {
        uhd::tx_metadata_t md;
        md.start_of_burst = true;
        md.end_of_burst   = true;
        md.has_time_spec  = true;
        md.time_spec      = usrp->get_time_now() - 0.010;   // in the past

        if (tx->send(samples.data(), samples.size(), md, 1.0) == samples.size())
            ++sent_ok;

        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        uhd::async_metadata_t am;
        while (tx->recv_async_msg(am, 0.05))
        {
            if (am.event_code == uhd::async_metadata_t::EVENT_CODE_TIME_ERROR)
                ++late;
            else if (am.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK)
                ++acks;
        }
    }

    std::cout
        << "\n  send() reported success : " << sent_ok << " of 10\n"
        << "  TIME_ERROR from device  : " << late << "\n"
        << "  BURST_ACK               : " << acks << "\n"
        << "\nThe gap between those first two numbers is the whole\n"
        << "lesson: send() succeeding is not evidence that anything\n"
        << "was transmitted. Only the async channel knows.\n";
}


// ============================================================
// Underflow: the U
// ============================================================

void demo_underflow(uhd::usrp::multi_usrp::sptr usrp, const Config& cfg)
{
    std::cout
        << "\n====================================================\n"
        << "3. UNDERFLOW  (expect: U, and UNDERFLOW)\n"
        << "====================================================\n"
        << "Opening a burst and then deliberately stalling. Once a\n"
        << "burst is open the device expects a continuous supply of\n"
        << "samples; if the host stops feeding it, the transmitter\n"
        << "runs dry mid-burst and reports UNDERFLOW.\n\n";

    auto tx = usrp->get_tx_stream(uhd::stream_args_t("fc32", otw_format()));
    const auto samples = make_waveform(4000, cfg.sample_rate, 10e3);

    uhd::tx_metadata_t md;
    md.start_of_burst = true;
    md.end_of_burst   = false;     // burst stays open
    md.has_time_spec  = false;

    size_t under = 0, others = 0;

    for (size_t i = 0; i < 5; ++i)
    {
        tx->send(samples.data(), samples.size(), md, 1.0);
        md.start_of_burst = false;

        /*
         * 4000 samples at 1 MS/s is 4 ms of audio. Sleeping 50 ms
         * leaves the device with nothing to play for 46 ms.
         */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        uhd::async_metadata_t am;
        while (tx->recv_async_msg(am, 0.05))
        {
            switch (am.event_code)
            {
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++under; break;
                default:
                    ++others; break;
            }
        }
    }

    md.end_of_burst = true;
    tx->send(samples.data(), 0, md);

    std::cout
        << "\n  UNDERFLOW = " << under << "\n"
        << "  other     = " << others << "\n"
        << "\nThe cure is to keep the burst fed, or to close it with\n"
        << "end_of_burst when you have nothing to send. A gap in an\n"
        << "open burst is an error; a closed burst is not.\n";
}


// ============================================================
// Overflow: the O
// ============================================================

void demo_overflow(uhd::usrp::multi_usrp::sptr usrp, const Config& cfg)
{
    std::cout
        << "\n====================================================\n"
        << "4. OVERFLOW  (expect: O, and ERROR_CODE_OVERFLOW)\n"
        << "====================================================\n"
        << "The receive-side mirror of underflow. The radio produces\n"
        << "samples at a fixed rate whether or not we collect them,\n"
        << "so stalling between recv() calls makes the buffers back\n"
        << "up and samples are dropped.\n\n";

    /*
     * Overflow is a race between the radio and the host, so at 1 MS/s
     * a modest stall is not enough to lose it -- the buffers absorb
     * the delay. Run this one fast, where falling behind is instant
     * and unmistakable.
     */
    const double demo_rate = 20e6;

    usrp->set_rx_rate(demo_rate);

    std::cout
        << "  (raising RX rate to "
        << usrp->get_rx_rate() / 1e6
        << " MS/s for this test -- at 1 MS/s the buffers absorb\n"
        << "   the stall and nothing is lost)\n\n";

    auto rx = usrp->get_rx_stream(uhd::stream_args_t("fc32", otw_format()));
    std::vector<complex_t> buf(rx->get_max_num_samps());

    uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    cmd.stream_now = true;
    rx->issue_stream_cmd(cmd);

    size_t over = 0, clean = 0;

    for (size_t i = 0; i < 40; ++i)
    {
        uhd::rx_metadata_t md;
        rx->recv(buf.data(), buf.size(), md, 1.0);

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
            ++over;
        else if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE)
            ++clean;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx->issue_stream_cmd(cmd);

    uhd::rx_metadata_t flush;
    while (rx->recv(buf.data(), buf.size(), flush, 0.1) > 0) {}

    usrp->set_rx_rate(cfg.sample_rate);   // put it back

    std::cout
        << "\n  clean recv() = " << clean << "\n"
        << "  OVERFLOW     = " << over << "\n"
        << "\nOverflow means samples are already gone -- there is no\n"
        << "way to get them back. Either read faster, use a lower\n"
        << "sample rate, or do the heavy work on another thread.\n";
}


// ============================================================
// Late stream command: LATE_COMMAND
// ============================================================

void demo_late_command(uhd::usrp::multi_usrp::sptr usrp, const Config& cfg)
{
    std::cout
        << "\n====================================================\n"
        << "5. LATE STREAM COMMAND  (expect: LATE_COMMAND)\n"
        << "====================================================\n"
        << "The receive-side equivalent of a late burst: asking the\n"
        << "device to start receiving at a time that has already\n"
        << "passed.\n\n";

    auto rx = usrp->get_rx_stream(uhd::stream_args_t("fc32", otw_format()));
    std::vector<complex_t> buf(rx->get_max_num_samps());

    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);

    cmd.num_samps  = 2000;
    cmd.stream_now = false;
    cmd.time_spec  = usrp->get_time_now() - 0.010;     // in the past

    rx->issue_stream_cmd(cmd);

    uhd::rx_metadata_t md;
    const size_t got = rx->recv(buf.data(), buf.size(), md, 1.0);

    std::cout
        << "  samples received = " << got << "\n"
        << "  error            = " << rx_error_to_string(md.error_code) << "\n"
        << "\nSame lesson as the late burst, on the other side: the\n"
        << "request was accepted, and nothing happened.\n";
}


// ============================================================
// Main
// ============================================================

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    Config cfg;

    const std::string mode = argc >= 2 ? argv[1] : "all";

    std::cout
        << "===============================================\n"
        << " UHD ASYNC EVENTS -- WHAT THE LETTERS MEAN\n"
        << "===============================================\n"
        << "Mode: " << mode << "\n";

    auto usrp = create_usrp(cfg);

    legend();

    if      (mode == "ack")       demo_ack(usrp, cfg);
    else if (mode == "late")      demo_late(usrp, cfg);
    else if (mode == "underflow") demo_underflow(usrp, cfg);
    else if (mode == "overflow")  demo_overflow(usrp, cfg);
    else if (mode == "latecmd")   demo_late_command(usrp, cfg);
    else if (mode == "all")
    {
        demo_ack(usrp, cfg);
        demo_late(usrp, cfg);
        demo_underflow(usrp, cfg);
        demo_overflow(usrp, cfg);
        demo_late_command(usrp, cfg);
    }
    else
    {
        std::cerr
            << "Unknown mode: " << mode << "\n\n"
            << "Usage:\n"
            << "  ./async_events ack         clean burst, nothing printed\n"
            << "  ./async_events late        provoke L / TIME_ERROR\n"
            << "  ./async_events underflow   provoke U / UNDERFLOW\n"
            << "  ./async_events overflow    provoke O / OVERFLOW\n"
            << "  ./async_events latecmd     provoke LATE_COMMAND\n"
            << "  ./async_events all\n";

        return 1;
    }

    std::cout << "\nDone.\n";

    return 0;
}
