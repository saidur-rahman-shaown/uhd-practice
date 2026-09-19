#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

/*
 * TDD slot timing on the B210.
 *
 * Two ways of laying out a frame, one of which does not work:
 *
 *   slots   one self-contained burst per slot. The obvious approach,
 *           and at a 1 ms cadence only about half the slots are ever
 *           acknowledged.
 *
 *   stream  one continuous burst, timestamped once, with the slot
 *           structure written into the samples. This holds a 1 ms
 *           cadence indefinitely.
 *
 * Both judge the outcome by the device's async reports rather than by
 * send() returning, because send() accepts a burst whose time has
 * already passed and the device then drops it without complaint.
 */


// ============================================================
// 10. Sustained TDD slot scheduling
// ============================================================

/*
 * The lead-time sweep measures one isolated burst at a time, which
 * flatters the host: it gets the whole inter-burst gap to prepare.
 * A TDD frame is the harder case -- slots land back to back at a
 * fixed cadence and the host must stay ahead of the clock for the
 * whole run.
 *
 * The right pattern is not to schedule each slot just in time.
 * Compute every slot's air time from one absolute start, queue
 * several slots ahead, and let the device's clock place them. The
 * host then only has to keep the pipeline full on average; it does
 * not have to hit each deadline individually.
 */

void run_tdd_slots(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double slot_ms = 1.0,
    size_t num_slots = 1000,
    double pipeline_ms = 20.0)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "10. SUSTAINED TDD SLOT SCHEDULING\n";
    std::cout << "====================================================\n";

    const size_t slot_samples =
        static_cast<size_t>(cfg.sample_rate * slot_ms / 1e3);

    const double slot_period = slot_ms / 1e3;

    std::cout
        << "\nSlot     = " << slot_ms << " ms ("
        << slot_samples << " samples)\n"
        << "Slots    = " << num_slots
        << "  (" << num_slots * slot_ms / 1e3 << " s of frame time)\n"
        << "Pipeline = " << pipeline_ms << " ms queued ahead\n\n";

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    const auto samples =
        make_waveform(slot_samples, cfg.sample_rate, 10e3);

    size_t acks = 0, late = 0, under = 0, other = 0, short_sends = 0;

    /*
     * One absolute anchor. Every slot is a fixed offset from it, so
     * the cadence cannot drift with host scheduling.
     */
    const uhd::time_spec_t start =
        usrp->get_time_now() + pipeline_ms / 1e3;

    auto drain = [&](double timeout) {
        uhd::async_metadata_t am;
        while (tx_stream->recv_async_msg(am, timeout)) {
            switch (am.event_code) {
                case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                    ++acks; break;
                case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                    ++late; break;
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++under; break;
                default:
                    ++other; break;
            }
        }
    };

    const auto wall_start = clock_type::now();

    for (size_t n = 0; n < num_slots; ++n)
    {
        uhd::tx_metadata_t md;

        md.start_of_burst = true;
        md.end_of_burst   = true;
        md.has_time_spec  = true;
        md.time_spec      = start + uhd::time_spec_t(double(n) * slot_period);

        const size_t sent =
            tx_stream->send(samples.data(), samples.size(), md, 1.0);

        if (sent != samples.size())
            ++short_sends;

        /*
         * Collect verdicts as they arrive, without blocking: the
         * device reports each burst well after it was queued.
         */
        drain(0.0);
    }

    /*
     * Let the tail of the frame play out, then take the rest.
     */
    std::this_thread::sleep_for(
        std::chrono::duration<double>(pipeline_ms / 1e3 + 0.2));

    drain(0.05);

    const double wall =
        std::chrono::duration<double>(
            clock_type::now() - wall_start).count();

    std::cout
        << "Result over " << num_slots << " slots:\n"
        << "  burst ACK    = " << acks << "\n"
        << "  late         = " << late << "\n"
        << "  underflow    = " << under << "\n"
        << "  other        = " << other << "\n"
        << "  short sends  = " << short_sends << "\n"
        << "  wall time    = " << std::fixed << std::setprecision(3)
        << wall << " s (frame time "
        << num_slots * slot_ms / 1e3 << " s)\n";

    const double ok =
        100.0 * double(acks) / double(num_slots);

    std::cout
        << "  on-time      = " << std::setprecision(2) << ok << " %\n";

    std::cout << "\nInterpretation:\n";

    if (late == 0 && under == 0 && acks == num_slots)
    {
        std::cout
            << "Every slot landed on time. A " << slot_ms
            << " ms TDD cadence is sustainable on this host at\n"
            << "this pipeline depth.\n";
    }
    else
    {
        std::cout
            << "Slots were missed. Raise the pipeline depth first --\n"
            << "queueing further ahead costs nothing but latency and\n"
            << "is the usual cure. Persistent underflow instead means\n"
            << "the host cannot generate samples fast enough.\n";
    }
}


// ============================================================
// 11. TDD as one continuous timed stream
// ============================================================

/*
 * Scheduling one burst per slot does not work at a 1 ms cadence on
 * this hardware: with start- and end-of-burst on every slot, only
 * about half of them are acknowledged. The device is being asked to
 * tear down and re-arm the transmit chain every slot, and it cannot
 * keep up.
 *
 * The approach that does work is to stop treating slots as separate
 * transmissions. Open the burst once, give it a single absolute
 * start time, and then stream continuously -- with the slot
 * structure written into the samples themselves, signal during the
 * transmit portion and zeros during the rest.
 *
 * Slot boundaries are then exact by construction, because they are
 * just sample counts inside one stream. The host has no per-slot
 * deadline to miss; it only has to keep the pipe fed, which is the
 * same job as any continuous transmission.
 */

void run_tdd_stream(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double slot_ms = 1.0,
    double seconds = 5.0,
    double duty = 0.5)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "11. TDD AS ONE CONTINUOUS TIMED STREAM\n";
    std::cout << "====================================================\n";

    const size_t slot_samples =
        static_cast<size_t>(cfg.sample_rate * slot_ms / 1e3);

    const size_t on_samples =
        static_cast<size_t>(double(slot_samples) * duty);

    std::cout
        << "\nSlot  = " << slot_ms << " ms (" << slot_samples
        << " samples), TX portion " << on_samples
        << " samples (" << duty * 100.0 << " %)\n"
        << "Run   = " << seconds << " s\n\n";

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    /*
     * One frame buffer holding a whole number of slots, sized to
     * keep the device fed comfortably.
     */
    const size_t slots_per_buffer =
        std::max<size_t>(1, 16000 / std::max<size_t>(1, slot_samples));

    const auto tone =
        make_waveform(on_samples, cfg.sample_rate, 10e3);

    std::vector<complex_t> buffer;
    buffer.reserve(slots_per_buffer * slot_samples);

    for (size_t s = 0; s < slots_per_buffer; ++s)
    {
        for (size_t n = 0; n < slot_samples; ++n)
        {
            buffer.push_back(
                n < on_samples
                    ? tone[n] * 0.7f
                    : complex_t(0.0f, 0.0f));
        }
    }

    std::cout
        << "Buffer = " << slots_per_buffer << " slots ("
        << buffer.size() << " samples per send)\n\n";

    size_t acks = 0, late = 0, under = 0, other = 0, sends = 0;

    uhd::tx_metadata_t md;

    md.start_of_burst = true;
    md.end_of_burst   = false;
    md.has_time_spec  = true;
    md.time_spec      = usrp->get_time_now() + 0.05;

    const auto t0 = clock_type::now();

    while (std::chrono::duration<double>(
               clock_type::now() - t0).count() < seconds)
    {
        tx_stream->send(buffer.data(), buffer.size(), md, 1.0);

        ++sends;

        md.start_of_burst = false;
        md.has_time_spec  = false;

        uhd::async_metadata_t am;

        while (tx_stream->recv_async_msg(am, 0.0))
        {
            switch (am.event_code)
            {
                case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                    ++acks; break;
                case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                    ++late; break;
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++under; break;
                default:
                    ++other; break;
            }
        }
    }

    md.end_of_burst = true;
    tx_stream->send(buffer.data(), 0, md);

    const size_t slots_sent = sends * slots_per_buffer;

    std::cout
        << "Result:\n"
        << "  sends        = " << sends << "\n"
        << "  slots        = " << slots_sent << "\n"
        << "  underflow    = " << under << "\n"
        << "  late         = " << late << "\n"
        << "  burst ACK    = " << acks << "\n"
        << "  other        = " << other << "\n";

    std::cout << "\nInterpretation:\n";

    if (under == 0 && late == 0)
    {
        std::cout
            << "No underflows and no late packets across "
            << slots_sent << " slots.\n"
            << "The " << slot_ms << " ms cadence holds: slot edges are\n"
            << "sample-exact inside the stream, so they cannot drift.\n"
            << "Only the first sample needed a timestamp.\n";
    }
    else if (under > 0)
    {
        std::cout
            << under << " underflows -- the host fell behind the\n"
            << "device. Send a larger buffer per call, or raise the\n"
            << "process priority.\n";
    }
    else
    {
        std::cout
            << late << " late packets, which should not happen once\n"
            << "the burst is open. Check the initial time spec.\n";
    }
}


// ============================================================
// 12. Alternating TX / RX slots
// ============================================================

/*
 * A real TDD frame: the same radio transmits in one slot and
 * listens in the next, TX-RX-TX-RX.
 *
 * Both directions run as continuous streams anchored to the same
 * device clock, never as one command per slot -- per-slot bursts
 * lose half their slots at a 1 ms cadence (see run_tdd_slots).
 * Instead the transmitter sends signal during its own slots and
 * zeros during the listening slots, and the receiver runs free,
 * with each sample assigned to a slot by its timestamp.
 *
 * Slot boundaries are therefore sample counts from a single
 * anchor, which is the only way to keep them from drifting.
 */

void run_tdd_alternating(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg_in,
    double slot_ms = 1.0,
    double seconds = 5.0,
    double guard_frac = 0.1,
    double tx_gain = 80.0,
    double rate = 0.0)
{
    usrp->set_tx_gain(tx_gain);

    /*
     * A TDD frame is only interesting at a rate that could carry a
     * real channel, so allow the sample rate to be pushed well past
     * the 1 MS/s default. Both directions run at once here, which is
     * the load that actually decides what the link can sustain.
     */
    Config cfg = cfg_in;

    if (rate > 0.0)
    {
        usrp->set_tx_rate(rate);
        usrp->set_rx_rate(rate);
        cfg.sample_rate = usrp->get_tx_rate();
    }

    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "12. ALTERNATING TX / RX SLOTS\n";
    std::cout << "====================================================\n";

    const size_t slot_samples =
        static_cast<size_t>(cfg.sample_rate * slot_ms / 1e3);

    const size_t guard_samples =
        static_cast<size_t>(double(slot_samples) * guard_frac);

    const double slot_period = slot_ms / 1e3;

    std::cout
        << "\nPattern : TX-RX-TX-RX, " << slot_ms << " ms per slot\n"
        << "Slot    : " << slot_samples << " samples\n"
        << "Guard   : " << guard_samples << " samples ("
        << guard_frac * 100.0 << " % at the end of each TX slot)\n"
        << "TX gain : " << usrp->get_tx_gain() << " dB\n"
        << "Rate    : " << cfg.sample_rate / 1e6 << " MS/s each way\n"
        << "Run     : " << seconds << " s\n\n";

    /*
     * Anchor both streams to one device time.
     */
    usrp->set_time_now(uhd::time_spec_t(0.0));

    auto tx = usrp->get_tx_stream(uhd::stream_args_t("fc32", "sc16"));
    auto rx = usrp->get_rx_stream(uhd::stream_args_t("fc32", "sc16"));

    /*
     * Two slots per buffer: one transmitting, one silent. The guard
     * at the end of the TX slot is the turnaround allowance -- the
     * power amplifier does not switch off instantly.
     */
    const auto tone =
        make_waveform(slot_samples, cfg.sample_rate, 10e3);

    std::vector<complex_t> frame;
    frame.reserve(slot_samples * 2);

    for (size_t n = 0; n < slot_samples; ++n)          // TX slot
    {
        frame.push_back(
            n < slot_samples - guard_samples
                ? tone[n] * 0.7f
                : complex_t(0.0f, 0.0f));
    }

    for (size_t n = 0; n < slot_samples; ++n)          // RX slot: silent
        frame.push_back(complex_t(0.0f, 0.0f));

    /*
     * Repeat the pair until the buffer is big enough to keep the
     * device fed without the host having to hit a deadline.
     */
    const size_t pairs =
        std::max<size_t>(1, 16000 / frame.size());

    std::vector<complex_t> tx_buffer;
    tx_buffer.reserve(frame.size() * pairs);

    for (size_t p = 0; p < pairs; ++p)
        tx_buffer.insert(tx_buffer.end(), frame.begin(), frame.end());

    const uhd::time_spec_t start(0.1);

    std::cout
        << "TX buffer = " << tx_buffer.size()
        << " samples (" << pairs * 2 << " slots per send)\n\n";

    std::atomic<size_t> underflows{0};
    std::atomic<bool> running{true};

    /*
     * Transmit from its own thread so the receive loop is never
     * blocked waiting on the transmitter.
     */
    std::thread tx_thread([&]() {
        uhd::tx_metadata_t md;
        md.start_of_burst = true;
        md.end_of_burst   = false;
        md.has_time_spec  = true;
        md.time_spec      = start;

        const auto t0 = clock_type::now();

        while (running.load()
               && std::chrono::duration<double>(
                      clock_type::now() - t0).count() < seconds)
        {
            tx->send(tx_buffer.data(), tx_buffer.size(), md, 1.0);

            md.start_of_burst = false;
            md.has_time_spec  = false;

            uhd::async_metadata_t am;
            while (tx->recv_async_msg(am, 0.0))
            {
                if (am.event_code
                        == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW
                    || am.event_code
                        == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET)
                    ++underflows;
            }
        }

        md.end_of_burst = true;
        tx->send(tx_buffer.data(), 0, md);
    });

    /*
     * Receive continuously and sort every sample into its slot by
     * timestamp, so we can see whether energy really lands only in
     * the transmitting slots.
     */
    uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    cmd.stream_now = false;
    cmd.time_spec  = start;
    rx->issue_stream_cmd(cmd);

    std::vector<complex_t> buf(rx->get_max_num_samps());

    double tx_slot_energy = 0.0, rx_slot_energy = 0.0;
    size_t tx_slot_n = 0, rx_slot_n = 0;
    size_t overflows = 0, no_time = 0;

    const auto t0 = clock_type::now();

    while (std::chrono::duration<double>(
               clock_type::now() - t0).count() < seconds)
    {
        uhd::rx_metadata_t md;

        const size_t got = rx->recv(buf.data(), buf.size(), md, 1.0);

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            ++overflows;
            continue;
        }

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE || got == 0)
            continue;

        if (!md.has_time_spec) { ++no_time; continue; }

        const double t_first = md.time_spec.get_real_secs();

        for (size_t n = 0; n < got; ++n)
        {
            const double t =
                t_first + double(n) / cfg.sample_rate;

            const double phase =
                (t - start.get_real_secs()) / slot_period;

            if (phase < 0.0) continue;

            const bool in_tx_slot =
                (static_cast<long long>(phase) % 2) == 0;

            const double e = std::norm(buf[n]);

            if (in_tx_slot) { tx_slot_energy += e; ++tx_slot_n; }
            else            { rx_slot_energy += e; ++rx_slot_n; }
        }
    }

    running.store(false);
    tx_thread.join();

    cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx->issue_stream_cmd(cmd);

    uhd::rx_metadata_t flush;
    while (rx->recv(buf.data(), buf.size(), flush, 0.1) > 0) {}

    const double tx_rms =
        tx_slot_n ? std::sqrt(tx_slot_energy / double(tx_slot_n)) : 0.0;
    const double rx_rms =
        rx_slot_n ? std::sqrt(rx_slot_energy / double(rx_slot_n)) : 0.0;

    std::cout
        << "Result:\n"
        << "  underflows        = " << underflows.load() << "\n"
        << "  overflows         = " << overflows << "\n"
        << "  samples in TX slots = " << tx_slot_n << "\n"
        << "  samples in RX slots = " << rx_slot_n << "\n"
        << std::scientific << std::setprecision(3)
        << "  rms during TX slots = " << tx_rms << "\n"
        << "  rms during RX slots = " << rx_rms << "\n";

    if (rx_rms > 0.0)
    {
        std::cout
            << std::fixed << std::setprecision(1)
            << "  on/off isolation    = "
            << 20.0 * std::log10(tx_rms / rx_rms) << " dB\n";
    }

    std::cout << "\nInterpretation:\n";

    if (underflows.load() == 0 && overflows == 0)
    {
        std::cout
            << "The frame held: no underflows, no overflows, both\n"
            << "directions streaming concurrently at a " << slot_ms
            << " ms cadence.\n";
    }
    else
    {
        std::cout
            << "The host could not sustain both directions at this\n"
            << "rate. Lower the sample rate or enlarge the buffers\n"
            << "before drawing conclusions about the timing.\n";
    }

    std::cout
        << "\nThe isolation figure is how far the listening slots sit\n"
        << "below the transmitting ones. On one radio with no external\n"
        << "loopback this is mostly TX leakage into the receiver, so\n"
        << "it bounds how quiet a listening slot can actually be.\n";
}


// ============================================================
// Main
// ============================================================

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    Config cfg;

    std::string mode = argc >= 2 ? argv[1] : "stream";

    std::cout
        << "===============================================\n"
        << " UHD B210 TDD SLOT TIMING\n"
        << "===============================================\n"
        << "Mode: " << mode << "\n";

    auto usrp = create_usrp(cfg);

    if (mode == "slots")
    {
        run_tdd_slots(
            usrp, cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? static_cast<size_t>(std::stoul(argv[3])) : 1000,
            argc >= 5 ? std::stod(argv[4]) : 20.0);
    }
    else if (mode == "stream")
    {
        run_tdd_stream(
            usrp, cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? std::stod(argv[3]) : 5.0,
            argc >= 5 ? std::stod(argv[4]) : 0.5);
    }
    else if (mode == "alt")
    {
        run_tdd_alternating(
            usrp, cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? std::stod(argv[3]) : 5.0,
            argc >= 5 ? std::stod(argv[4]) : 0.1,
            argc >= 6 ? std::stod(argv[5]) : 80.0,
            argc >= 7 ? std::stod(argv[6]) : 0.0);
    }
    else if (mode == "both")
    {
        run_tdd_slots(usrp, cfg, 1.0, 1000, 20.0);
        run_tdd_stream(usrp, cfg, 1.0, 5.0, 0.5);
    }
    else
    {
        std::cerr
            << "Unknown mode: " << mode << "\n\n"
            << "Usage:\n"
            << "  ./tdd_latency_test slots  [slot_ms] [slots] [pipeline_ms]\n"
            << "  ./tdd_latency_test stream [slot_ms] [seconds] [duty]\n"
            << "  ./tdd_latency_test alt    [slot_ms] [seconds] [guard_frac] [tx_gain] [rate]\n"
            << "  ./tdd_latency_test both\n";

        return 1;
    }

    std::cout << "\nExperiment complete.\n";

    return 0;
}
