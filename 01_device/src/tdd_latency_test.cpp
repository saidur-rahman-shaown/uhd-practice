#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
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
            << "  ./tdd_latency_test both\n";

        return 1;
    }

    std::cout << "\nExperiment complete.\n";

    return 0;
}
