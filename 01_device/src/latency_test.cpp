#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <fstream>
#include <vector>

using clock_type = std::chrono::steady_clock;
using complex_t = std::complex<float>;


// ============================================================
// Configuration
// ============================================================

struct Config
{
    double frequency = 3515e6;
    double sample_rate = 1e6;

    double tx_gain = 20.0;
    double rx_gain = 30.0;

    size_t num_samples = 4096;

    size_t channel = 0;

    std::string args = "";
};


// ============================================================
// Statistics
// ============================================================

struct Statistics
{
    double min = 0;
    double max = 0;
    double mean = 0;
    double median = 0;
};


Statistics calculate_stats(std::vector<double> values)
{
    Statistics s{};

    if (values.empty())
        return s;

    std::sort(values.begin(), values.end());

    s.min = values.front();
    s.max = values.back();

    s.mean =
        std::accumulate(values.begin(), values.end(), 0.0)
        / static_cast<double>(values.size());

    if (values.size() % 2 == 0)
    {
        s.median =
            (values[values.size() / 2 - 1]
             + values[values.size() / 2]) / 2.0;
    }
    else
    {
        s.median = values[values.size() / 2];
    }

    return s;
}


// ============================================================
// Create and configure USRP
// ============================================================

uhd::usrp::multi_usrp::sptr create_usrp(const Config& cfg)
{
    std::cout << "\nCreating USRP...\n";

    auto usrp = uhd::usrp::multi_usrp::make(cfg.args);

    std::cout << "Device:\n";
    std::cout << usrp->get_pp_string() << "\n";

    usrp->set_rx_rate(cfg.sample_rate);
    usrp->set_tx_rate(cfg.sample_rate);

    usrp->set_rx_freq(cfg.frequency);
    usrp->set_tx_freq(cfg.frequency);

    usrp->set_rx_gain(cfg.rx_gain);
    usrp->set_tx_gain(cfg.tx_gain);

    std::cout << "\nConfiguration:\n";
    std::cout << "  Frequency : " << cfg.frequency / 1e6 << " MHz\n";
    std::cout << "  Sample rate: " << cfg.sample_rate / 1e6 << " MS/s\n";
    std::cout << "  TX gain   : " << cfg.tx_gain << " dB\n";
    std::cout << "  RX gain   : " << cfg.rx_gain << " dB\n";

    return usrp;
}


// ============================================================
// Create a simple complex sinusoid
// ============================================================

std::vector<complex_t> make_waveform(
    size_t num_samples,
    double sample_rate,
    double tone_frequency)
{
    std::vector<complex_t> samples(num_samples);

    const double two_pi = 2.0 * M_PI;

    for (size_t n = 0; n < num_samples; ++n)
    {
        double phase =
            two_pi * tone_frequency
            * static_cast<double>(n)
            / sample_rate;

        samples[n] =
            complex_t(
                static_cast<float>(std::cos(phase)),
                static_cast<float>(std::sin(phase)));
    }

    return samples;
}


// ============================================================
// 1. Measure send() execution time
// ============================================================

void measure_send_execution(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "1. MEASURE send() EXECUTION TIME\n";
    std::cout << "====================================================\n";

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto samples =
        make_waveform(
            cfg.num_samples,
            cfg.sample_rate,
            10e3);

    std::vector<double> durations_us;

    const size_t iterations = 100;

    for (size_t i = 0; i < iterations; ++i)
    {
        uhd::tx_metadata_t md;

        md.start_of_burst = true;
        md.end_of_burst = true;

        md.has_time_spec = false;

        auto t0 = clock_type::now();

        tx_stream->send(
            samples.data(),
            samples.size(),
            md);

        auto t1 = clock_type::now();

        double elapsed_us =
            std::chrono::duration<double, std::micro>(
                t1 - t0).count();

        durations_us.push_back(elapsed_us);
    }

    auto stats = calculate_stats(durations_us);

    std::cout << std::fixed << std::setprecision(3);

    std::cout << "\nsend() host execution time:\n";
    std::cout << "  min    = " << stats.min << " us\n";
    std::cout << "  median = " << stats.median << " us\n";
    std::cout << "  mean   = " << stats.mean << " us\n";
    std::cout << "  max    = " << stats.max << " us\n";

    std::cout << "\nIMPORTANT:\n";
    std::cout
        << "This is the host-side duration of send().\n"
        << "It is NOT yet the RF transmission latency.\n";
}


// ============================================================
// Helper: send one timed burst
// ============================================================

bool send_timed_burst(
    uhd::tx_streamer::sptr tx_stream,
    const std::vector<complex_t>& samples,
    uhd::time_spec_t tx_time,
    double timeout = 1.0)
{
    uhd::tx_metadata_t md;

    md.start_of_burst = true;
    md.end_of_burst = true;

    md.has_time_spec = true;
    md.time_spec = tx_time;

    size_t sent =
        tx_stream->send(
            samples.data(),
            samples.size(),
            md,
            timeout);

    return sent == samples.size();
}


// ============================================================
// 2 + 3. Minimum safe lead time, judged by async metadata
// ============================================================

/*
 * send() returning the full sample count only means the host
 * handed the samples to UHD. It says nothing about whether the
 * device transmitted them at the requested time -- a burst whose
 * time_spec has already passed is dropped and reported later, out
 * of band, as EVENT_CODE_TIME_ERROR.
 *
 * So schedule each burst, then drain the async channel and let the
 * device say what actually happened.
 */

struct LeadResult
{
    size_t acks = 0;
    size_t time_errors = 0;
    size_t underflows = 0;
    size_t other = 0;
    size_t no_message = 0;
};


LeadResult probe_lead_time(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double lead_seconds,
    size_t slot_samples,
    size_t iterations)
{
    LeadResult r{};

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    const auto samples =
        make_waveform(slot_samples, cfg.sample_rate, 10e3);

    const double slot_seconds =
        static_cast<double>(slot_samples) / cfg.sample_rate;

    for (size_t i = 0; i < iterations; ++i)
    {
        uhd::tx_metadata_t md;

        md.start_of_burst = true;
        md.end_of_burst   = true;
        md.has_time_spec  = true;
        md.time_spec      = usrp->get_time_now() + lead_seconds;

        tx_stream->send(
            samples.data(),
            samples.size(),
            md,
            1.0);

        /*
         * Wait for the burst to play out, then collect the verdict.
         */
        std::this_thread::sleep_for(
            std::chrono::duration<double>(
                lead_seconds + slot_seconds + 0.002));

        uhd::async_metadata_t am;

        bool got = false;

        while (tx_stream->recv_async_msg(am, 0.05))
        {
            got = true;

            switch (am.event_code)
            {
                case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                    ++r.acks;
                    break;

                case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                    ++r.time_errors;
                    break;

                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++r.underflows;
                    break;

                default:
                    ++r.other;
                    break;
            }
        }

        if (!got)
            ++r.no_message;
    }

    return r;
}


void find_minimum_safe_lead(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double slot_ms = 1.0,
    size_t iterations = 50)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "2 + 3. MINIMUM SAFE LEAD TIME (async-verified)\n";
    std::cout << "====================================================\n";

    const size_t slot_samples =
        static_cast<size_t>(cfg.sample_rate * slot_ms / 1e3);

    std::cout
        << "\nSlot = " << slot_ms << " ms ("
        << slot_samples << " samples at "
        << cfg.sample_rate / 1e6 << " MS/s), "
        << iterations << " bursts per lead time.\n\n";

    std::cout
        << "  lead      ACK   late  under   none   on-time\n"
        << "  ---------------------------------------------\n";

    const std::vector<double> leads_ms =
        {0.2, 0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0, 30.0};

    double best = -1.0;

    for (double lead_ms : leads_ms)
    {
        usrp->clear_command_time();

        const auto r =
            probe_lead_time(
                usrp, cfg, lead_ms / 1e3, slot_samples, iterations);

        const double rate =
            100.0 * double(r.acks) / double(iterations);

        std::cout
            << "  " << std::setw(5) << std::fixed
            << std::setprecision(1) << lead_ms << " ms"
            << std::setw(7) << r.acks
            << std::setw(7) << r.time_errors
            << std::setw(7) << r.underflows
            << std::setw(7) << r.no_message
            << std::setw(9) << std::setprecision(1) << rate << " %"
            << "\n";

        if (r.acks == iterations && r.time_errors == 0 && best < 0.0)
            best = lead_ms;
    }

    std::cout << "\n";

    if (best > 0.0)
    {
        std::cout
            << "Minimum lead time with no late bursts over "
            << iterations << " trials: " << best << " ms.\n\n"
            << "Budget more than this in practice -- this is the\n"
            << "shortest lead that happened to be clean here, not a\n"
            << "guaranteed bound. Host scheduling jitter under load\n"
            << "will push it up.\n";
    }
    else
    {
        std::cout
            << "No lead time in the sweep was completely clean.\n"
            << "If even the longest lead shows late bursts, the host\n"
            << "is not keeping up at all -- check for CPU contention.\n";
    }

    std::cout
        << "\nFor TDD slot timing: the lead time is how far ahead of\n"
        << "air time the host must hand a burst to UHD. A slot period\n"
        << "shorter than this cannot be scheduled per-slot from the\n"
        << "host; the schedule has to be built further ahead, as a\n"
        << "run of bursts queued in advance with absolute timestamps.\n";
}


// ============================================================
// RX helper
// ============================================================

void start_timed_rx(
    uhd::rx_streamer::sptr rx_stream,
    uhd::time_spec_t rx_time,
    size_t num_samples)
{
    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);

    cmd.num_samps = num_samples;

    cmd.stream_now = false;
    cmd.time_spec = rx_time;

    rx_stream->issue_stream_cmd(cmd);
}


// ============================================================
// 4. TX -> RX using timestamps
// ============================================================

void measure_tx_rx_timestamp_latency(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "4. TX -> RX USING DEVICE TIMESTAMPS\n";
    std::cout << "====================================================\n";

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto tx_samples =
        make_waveform(
            cfg.num_samples,
            cfg.sample_rate,
            10e3);

    std::vector<complex_t> rx_samples(
        rx_stream->get_max_num_samps());

    /*
     * We perform several measurements.
     */
    const size_t iterations = 20;

    std::vector<double> latency_us;

    for (size_t i = 0; i < iterations; ++i)
    {
        /*
         * Get current device time.
         */
        const uhd::time_spec_t now =
            usrp->get_time_now();

        /*
         * Give the host/USB path some time to prepare.
         */
        const double lead_time = 0.020;

        const uhd::time_spec_t tx_time =
            now + lead_time;

        /*
         * Start RX slightly before TX.
         *
         * We deliberately capture a window around TX.
         */
        const double rx_lead = 0.005;

        const uhd::time_spec_t rx_time =
            tx_time - rx_lead;

        start_timed_rx(
            rx_stream,
            rx_time,
            cfg.num_samples);

        /*
         * Schedule TX.
         */
        uhd::tx_metadata_t tx_md;

        tx_md.start_of_burst = true;
        tx_md.end_of_burst = true;

        tx_md.has_time_spec = true;
        tx_md.time_spec = tx_time;

        tx_stream->send(
            tx_samples.data(),
            tx_samples.size(),
            tx_md);

        /*
         * Receive.
         */
        uhd::rx_metadata_t rx_md;

        size_t received =
            rx_stream->recv(
                rx_samples.data(),
                rx_samples.size(),
                rx_md,
                1.0);

        if (rx_md.error_code !=
            uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            std::cout
                << "RX error: "
                << rx_md.strerror()
                << "\n";

            continue;
        }

        if (!rx_md.has_time_spec)
        {
            std::cout
                << "RX packet has no timestamp\n";

            continue;
        }

        /*
         * Timestamp of first received sample.
         */
        const uhd::time_spec_t rx_timestamp =
            rx_md.time_spec;

        /*
         * Device-time difference.
         */
        double delta =
            (rx_timestamp - tx_time)
                .get_real_secs();

        double delta_us =
            delta * 1e6;

        latency_us.push_back(delta_us);

        std::cout
            << std::fixed
            << std::setprecision(3)
            << "Iteration "
            << std::setw(2)
            << i
            << ": "
            << "TX = "
            << tx_time.get_real_secs()
            << " s, RX = "
            << rx_timestamp.get_real_secs()
            << " s, delta = "
            << delta_us
            << " us, samples = "
            << received
            << "\n";
    }

    auto stats =
        calculate_stats(latency_us);

    std::cout << "\nTX -> RX device timestamp latency:\n";

    std::cout
        << "  min    = "
        << stats.min
        << " us\n";

    std::cout
        << "  median = "
        << stats.median
        << " us\n";

    std::cout
        << "  mean   = "
        << stats.mean
        << " us\n";

    std::cout
        << "  max    = "
        << stats.max
        << " us\n";

    std::cout
        << "\nInterpretation:\n";

    std::cout
        << "This is approximately the delay between the\n"
        << "scheduled TX sample time and the timestamp of\n"
        << "the first RX sample observed by UHD.\n";
}


// ============================================================
// 5. Separate host RX/TX contribution
// ============================================================

void measure_host_recv_time(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "5. HOST recv() EXECUTION TIME\n";
    std::cout << "====================================================\n";

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    std::vector<complex_t> buffer(
        rx_stream->get_max_num_samps());

    /*
     * Continuous RX.
     */
    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    cmd.stream_now = true;

    rx_stream->issue_stream_cmd(cmd);

    const size_t iterations = 100;

    std::vector<double> durations_us;

    for (size_t i = 0; i < iterations; ++i)
    {
        uhd::rx_metadata_t md;

        auto t0 = clock_type::now();

        size_t received =
            rx_stream->recv(
                buffer.data(),
                buffer.size(),
                md,
                1.0);

        auto t1 = clock_type::now();

        if (received == 0)
            continue;

        double elapsed_us =
            std::chrono::duration<double, std::micro>(
                t1 - t0).count();

        durations_us.push_back(elapsed_us);
    }

    /*
     * Stop RX.
     */
    cmd.stream_mode =
        uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

    rx_stream->issue_stream_cmd(cmd);

    auto stats =
        calculate_stats(durations_us);

    std::cout
        << std::fixed
        << std::setprecision(3);

    std::cout
        << "\nrecv() duration:\n";

    std::cout
        << "  min    = "
        << stats.min
        << " us\n";

    std::cout
        << "  median = "
        << stats.median
        << " us\n";

    std::cout
        << "  mean   = "
        << stats.mean
        << " us\n";

    std::cout
        << "  max    = "
        << stats.max
        << " us\n";

    std::cout
        << "\nThis measures host-side recv() duration.\n"
        << "It does not isolate USB transfer time yet.\n";
}


// ============================================================
// Helper: decode UHD async (TX) event codes
// ============================================================

const char* async_event_to_string(
    uhd::async_metadata_t::event_code_t code)
{
    switch (code)
    {
        case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
            return "BURST_ACK (burst transmitted successfully)";

        case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
            return "UNDERFLOW (host did not feed samples fast enough)";

        case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
            return "UNDERFLOW_IN_PACKET (underflow inside a packet)";

        case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
            return "SEQ_ERROR (packet sequence error)";

        case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
            return "SEQ_ERROR_IN_BURST (sequence error inside a burst)";

        case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
            return "TIME_ERROR (late packet: time_spec already passed)";

        case uhd::async_metadata_t::EVENT_CODE_USER_PAYLOAD:
            return "USER_PAYLOAD";

        default:
            return "UNKNOWN";
    }
}


// ============================================================
// Helper: decode RX metadata error codes
// ============================================================

const char* rx_error_to_string(
    uhd::rx_metadata_t::error_code_t code)
{
    switch (code)
    {
        case uhd::rx_metadata_t::ERROR_CODE_NONE:
            return "NONE (no error)";

        case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
            return "TIMEOUT (no samples before the recv timeout)";

        case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
            return "LATE_COMMAND (stream command time already passed)";

        case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
            return "BROKEN_CHAIN (expected another stream command)";

        case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
            return "OVERFLOW (host did not drain samples fast enough)";

        case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
            return "ALIGNMENT (multi-channel alignment failure)";

        case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
            return "BAD_PACKET (malformed packet)";

        default:
            return "UNKNOWN";
    }
}


// ============================================================
// 6. TX async metadata: underflow / late / sequence errors
// ============================================================

void monitor_tx_metadata(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "6. TX ASYNC METADATA (UNDERFLOW / LATE / SEQ)\n";
    std::cout << "====================================================\n";

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto samples =
        make_waveform(
            cfg.num_samples,
            cfg.sample_rate,
            10e3);

    size_t num_ack = 0;
    size_t num_underflow = 0;
    size_t num_time_error = 0;
    size_t num_seq_error = 0;
    size_t num_other = 0;
    size_t num_no_message = 0;

    const size_t iterations = 20;

    /*
     * Two cases are exercised on purpose:
     *
     *   - a healthy burst scheduled into the future
     *   - a burst scheduled in the past, which the device must
     *     report as EVENT_CODE_TIME_ERROR (a "late" packet)
     */
    for (size_t i = 0; i < iterations; ++i)
    {
        const bool make_it_late = (i % 4 == 3);

        const uhd::time_spec_t now =
            usrp->get_time_now();

        const double lead_time =
            make_it_late ? -0.010 : 0.020;

        const uhd::time_spec_t tx_time =
            now + lead_time;

        uhd::tx_metadata_t md;

        md.start_of_burst = true;
        md.end_of_burst = true;

        md.has_time_spec = true;
        md.time_spec = tx_time;

        tx_stream->send(
            samples.data(),
            samples.size(),
            md);

        /*
         * Drain every async message produced for this burst.
         * recv_async_msg() returns false when nothing arrived
         * before the timeout.
         */
        uhd::async_metadata_t async_md;

        bool got_any = false;

        while (tx_stream->recv_async_msg(async_md, 0.2))
        {
            got_any = true;

            switch (async_md.event_code)
            {
                case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                    ++num_ack;
                    break;

                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++num_underflow;
                    break;

                case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                    ++num_time_error;
                    break;

                case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
                case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
                    ++num_seq_error;
                    break;

                default:
                    ++num_other;
                    break;
            }

            std::cout
                << "Iteration "
                << std::setw(2)
                << i
                << (make_it_late
                        ? " [late on purpose]: "
                        : "                  : ")
                << async_event_to_string(async_md.event_code);

            if (async_md.has_time_spec)
            {
                std::cout
                    << ", event time = "
                    << std::fixed
                    << std::setprecision(6)
                    << async_md.time_spec.get_real_secs()
                    << " s";
            }

            std::cout << "\n";
        }

        if (!got_any)
        {
            ++num_no_message;

            std::cout
                << "Iteration "
                << std::setw(2)
                << i
                << "                  : "
                << "no async message within timeout\n";
        }

        /*
         * Let the scheduled burst play out before the next one.
         */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(30));
    }

    std::cout
        << "\nTX async event summary over "
        << iterations
        << " bursts:\n";

    std::cout << "  burst ACK         = " << num_ack << "\n";
    std::cout << "  underflow         = " << num_underflow << "\n";
    std::cout << "  time error (late) = " << num_time_error << "\n";
    std::cout << "  sequence error    = " << num_seq_error << "\n";
    std::cout << "  other events      = " << num_other << "\n";
    std::cout << "  no message        = " << num_no_message << "\n";

    std::cout
        << "\nInterpretation:\n"
        << "UNDERFLOW means the host could not feed the TX chain\n"
        << "fast enough. TIME_ERROR means the burst reached the\n"
        << "device after its time_spec had passed, so it was\n"
        << "dropped. The deliberately late iterations should show\n"
        << "up as TIME_ERROR.\n";
}


// ============================================================
// 7. RX metadata: overflow / late command / timeout
// ============================================================

void monitor_rx_metadata(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "7. RX METADATA (OVERFLOW / LATE COMMAND / TIMEOUT)\n";
    std::cout << "====================================================\n";

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    std::vector<complex_t> buffer(
        rx_stream->get_max_num_samps());

    /*
     * Part A: a stream command scheduled in the past must come
     * back as ERROR_CODE_LATE_COMMAND.
     */
    std::cout << "\nA. Deliberately late stream command:\n";

    {
        const uhd::time_spec_t past =
            usrp->get_time_now() - 0.010;

        start_timed_rx(
            rx_stream,
            past,
            cfg.num_samples);

        uhd::rx_metadata_t md;

        size_t received =
            rx_stream->recv(
                buffer.data(),
                buffer.size(),
                md,
                1.0);

        std::cout
            << "  received = "
            << received
            << ", error = "
            << rx_error_to_string(md.error_code)
            << "\n";
    }

    /*
     * Part B: continuous streaming with a host that is
     * deliberately too slow, which provokes overflows.
     */
    std::cout << "\nB. Continuous RX with a slow consumer:\n";

    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    cmd.stream_now = true;

    rx_stream->issue_stream_cmd(cmd);

    size_t num_none = 0;
    size_t num_overflow = 0;
    size_t num_timeout = 0;
    size_t num_late = 0;
    size_t num_other = 0;

    size_t total_samples = 0;

    size_t num_fragments = 0;
    size_t num_no_time_spec = 0;

    const size_t iterations = 100;

    for (size_t i = 0; i < iterations; ++i)
    {
        uhd::rx_metadata_t md;

        size_t received =
            rx_stream->recv(
                buffer.data(),
                buffer.size(),
                md,
                1.0);

        total_samples += received;

        if (md.more_fragments)
            ++num_fragments;

        if (!md.has_time_spec)
            ++num_no_time_spec;

        switch (md.error_code)
        {
            case uhd::rx_metadata_t::ERROR_CODE_NONE:
                ++num_none;
                break;

            case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
                ++num_overflow;
                break;

            case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
                ++num_timeout;
                break;

            case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
                ++num_late;
                break;

            default:
                ++num_other;
                break;
        }

        if (md.error_code !=
            uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            std::cout
                << "  iteration "
                << std::setw(3)
                << i
                << ": "
                << rx_error_to_string(md.error_code)
                << (md.out_of_sequence
                        ? " [out of sequence]"
                        : "")
                << "\n";
        }

        /*
         * Stall the consumer so the buffers fill up. This is
         * what makes overflows observable.
         */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }

    cmd.stream_mode =
        uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

    rx_stream->issue_stream_cmd(cmd);

    /*
     * Flush whatever is still in flight after the stop.
     */
    {
        uhd::rx_metadata_t md;

        while (rx_stream->recv(
                   buffer.data(),
                   buffer.size(),
                   md,
                   0.1) > 0)
        {
        }
    }

    std::cout
        << "\nRX metadata summary over "
        << iterations
        << " recv() calls:\n";

    std::cout << "  no error      = " << num_none << "\n";
    std::cout << "  overflow      = " << num_overflow << "\n";
    std::cout << "  timeout       = " << num_timeout << "\n";
    std::cout << "  late command  = " << num_late << "\n";
    std::cout << "  other errors  = " << num_other << "\n";
    std::cout << "  total samples = " << total_samples << "\n";
    std::cout << "  fragmented    = " << num_fragments << "\n";
    std::cout << "  no time spec  = " << num_no_time_spec << "\n";

    std::cout
        << "\nInterpretation:\n"
        << "OVERFLOW means the device produced samples faster than\n"
        << "this program drained them, so data was dropped (the\n"
        << "classic 'O' that UHD prints).\n"
        << "LATE_COMMAND means the stream command's time_spec was\n"
        << "already in the past when the device saw it.\n";
}


// ============================================================
// Zadoff-Chu sequence generation
// ============================================================

/*
 * A Zadoff-Chu sequence of length N with root u is
 *
 *     x[n] = exp(-j * pi * u * n * (n + 1) / N)     (N odd)
 *
 * It is constant modulus, and its periodic autocorrelation is
 * an impulse: zero at every non-zero lag. That is what makes it
 * a good synchronisation burst -- correlating a capture against
 * a known ZC gives one sharp peak at the delay.
 *
 * N should be prime and u coprime to N, which keeps the ideal
 * correlation property.
 */

std::vector<complex_t> make_zadoff_chu(
    size_t length,
    size_t root)
{
    if (length == 0)
        return {};

    if (std::gcd(root, length) != 1)
    {
        std::cerr
            << "WARNING: ZC root "
            << root
            << " is not coprime to length "
            << length
            << "; correlation will not be ideal.\n";
    }

    std::vector<complex_t> seq(length);

    for (size_t n = 0; n < length; ++n)
    {
        /*
         * exp(-j*pi*k/N) repeats every k = 2N, so reduce the
         * numerator first. Without this the phase argument grows
         * large enough to lose precision for long sequences.
         */
        const unsigned long long num =
            static_cast<unsigned long long>(root)
            * static_cast<unsigned long long>(n)
            * static_cast<unsigned long long>(n + 1);

        const unsigned long long reduced =
            num % (2ULL * static_cast<unsigned long long>(length));

        const double phase =
            -M_PI
            * static_cast<double>(reduced)
            / static_cast<double>(length);

        seq[n] =
            complex_t(
                static_cast<float>(std::cos(phase)),
                static_cast<float>(std::sin(phase)));
    }

    return seq;
}


// ============================================================
// Zadoff-Chu detection by cross-correlation
// ============================================================

struct ZcDetection
{
    bool found = false;

    size_t peak_index = 0;

    double peak_magnitude = 0.0;
    double mean_magnitude = 0.0;

    /*
     * Peak-to-average ratio of the correlation magnitude. This is
     * the detection metric: a clean hit sits far above the noise
     * floor, a miss hovers near 1.
     */
    double peak_to_mean = 0.0;
};


/*
 * Slide the reference sequence over the capture and find the lag
 * with the largest correlation magnitude.
 *
 * This is a direct O(N*M) correlation. For the burst lengths used
 * here that is cheap and much easier to read than an FFT version.
 */
ZcDetection detect_zadoff_chu(
    const std::vector<complex_t>& capture,
    const std::vector<complex_t>& reference,
    double threshold = 8.0)
{
    ZcDetection result{};

    if (reference.empty()
        || capture.size() < reference.size())
    {
        return result;
    }

    const size_t num_lags =
        capture.size() - reference.size() + 1;

    double sum_magnitude = 0.0;

    for (size_t lag = 0; lag < num_lags; ++lag)
    {
        complex_t acc(0.0f, 0.0f);

        for (size_t k = 0; k < reference.size(); ++k)
        {
            acc += capture[lag + k]
                 * std::conj(reference[k]);
        }

        const double magnitude = std::abs(acc);

        sum_magnitude += magnitude;

        if (magnitude > result.peak_magnitude)
        {
            result.peak_magnitude = magnitude;
            result.peak_index = lag;
        }
    }

    result.mean_magnitude =
        sum_magnitude / static_cast<double>(num_lags);

    if (result.mean_magnitude > 0.0)
    {
        result.peak_to_mean =
            result.peak_magnitude / result.mean_magnitude;
    }

    result.found =
        result.peak_to_mean >= threshold;

    return result;
}


// ============================================================
// 8. Zadoff-Chu generate + detect
// ============================================================

/*
 * Offline positive control: build a capture with the sequence
 * planted at a known offset, then check the detector finds it
 * there. This needs no hardware, so a failure here is a bug in
 * the correlator rather than an RF problem.
 */
bool zc_self_test()
{
    std::cout << "\nA. Offline self-test (no hardware):\n";

    const size_t zc_length = 401;
    const size_t zc_root = 25;

    const auto zc =
        make_zadoff_chu(zc_length, zc_root);

    /*
     * Check the constant-modulus property.
     */
    double min_mag = 1e9;
    double max_mag = 0.0;

    for (const auto& s : zc)
    {
        const double m = std::abs(s);

        min_mag = std::min(min_mag, m);
        max_mag = std::max(max_mag, m);
    }

    std::cout
        << "  sequence length     = " << zc_length << "\n"
        << "  root                = " << zc_root << "\n"
        << std::fixed << std::setprecision(6)
        << "  sample magnitude    = ["
        << min_mag << ", " << max_mag << "]  (expect ~1.0)\n";

    /*
     * Plant the sequence at a known offset inside noise-free zeros.
     */
    const size_t planted_offset = 1234;

    std::vector<complex_t> capture(
        planted_offset + zc_length + 500,
        complex_t(0.0f, 0.0f));

    for (size_t k = 0; k < zc_length; ++k)
        capture[planted_offset + k] = zc[k];

    const auto det =
        detect_zadoff_chu(capture, zc);

    std::cout
        << std::setprecision(2)
        << "  planted at offset   = " << planted_offset << "\n"
        << "  detected at offset  = " << det.peak_index << "\n"
        << "  peak/mean           = " << det.peak_to_mean << "\n"
        << "  detected            = "
        << (det.found ? "yes" : "no") << "\n";

    const bool ok =
        det.found && det.peak_index == planted_offset;

    std::cout
        << "  SELF-TEST           = "
        << (ok ? "PASS" : "FAIL")
        << "\n";

    return ok;
}


/*
 * Live test: transmit a ZC burst at a scheduled device time,
 * capture a window around it, and correlate to find exactly where
 * it landed.
 *
 * Unlike reading rx_md.time_spec (which only reports when the RX
 * window opened), this recovers the arrival time of the burst
 * itself, so the TX -> RX delay it prints is a real measurement.
 */
void run_zadoff_chu_test(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "8. ZADOFF-CHU GENERATE + DETECT\n";
    std::cout << "====================================================\n";

    if (!zc_self_test())
    {
        std::cout
            << "\nSelf-test failed, skipping the live test.\n";

        return;
    }

    std::cout << "\nB. Live TX -> RX detection:\n";

    const size_t zc_length = 401;
    const size_t zc_root = 25;

    const auto zc =
        make_zadoff_chu(zc_length, zc_root);

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    /*
     * Capture window: open it before TX and keep it open past the
     * burst, so the sequence is somewhere inside.
     */
    const double rx_lead = 0.002;

    const size_t capture_len =
        static_cast<size_t>(cfg.sample_rate * 0.008);

    const size_t iterations = 10;

    std::vector<double> delays_us;

    for (size_t i = 0; i < iterations; ++i)
    {
        const uhd::time_spec_t now =
            usrp->get_time_now();

        const uhd::time_spec_t tx_time =
            now + 0.020;

        const uhd::time_spec_t rx_time =
            tx_time - rx_lead;

        start_timed_rx(
            rx_stream,
            rx_time,
            capture_len);

        uhd::tx_metadata_t tx_md;

        tx_md.start_of_burst = true;
        tx_md.end_of_burst = true;

        tx_md.has_time_spec = true;
        tx_md.time_spec = tx_time;

        tx_stream->send(
            zc.data(),
            zc.size(),
            tx_md);

        /*
         * Drain the whole capture window into one buffer, keeping
         * the timestamp of the very first sample so a correlation
         * index can be turned back into device time.
         */
        std::vector<complex_t> capture;
        capture.reserve(capture_len);

        uhd::time_spec_t first_sample_time;

        bool have_first = false;
        bool rx_error = false;

        std::vector<complex_t> chunk(
            rx_stream->get_max_num_samps());

        while (capture.size() < capture_len)
        {
            uhd::rx_metadata_t rx_md;

            const size_t got =
                rx_stream->recv(
                    chunk.data(),
                    chunk.size(),
                    rx_md,
                    1.0);

            if (rx_md.error_code !=
                uhd::rx_metadata_t::ERROR_CODE_NONE)
            {
                std::cout
                    << "  iteration "
                    << std::setw(2) << i
                    << ": RX error: "
                    << rx_error_to_string(rx_md.error_code)
                    << "\n";

                rx_error = true;
                break;
            }

            if (got == 0)
                break;

            if (!have_first && rx_md.has_time_spec)
            {
                first_sample_time = rx_md.time_spec;
                have_first = true;
            }

            capture.insert(
                capture.end(),
                chunk.begin(),
                chunk.begin() + got);
        }

        if (rx_error || !have_first || capture.size() < zc_length)
            continue;

        const auto det =
            detect_zadoff_chu(capture, zc);

        if (!det.found)
        {
            std::cout
                << "  iteration "
                << std::setw(2) << i
                << ": no ZC detected (peak/mean = "
                << std::fixed << std::setprecision(2)
                << det.peak_to_mean
                << ")\n";

            continue;
        }

        /*
         * Convert the correlation index back into device time.
         */
        const uhd::time_spec_t arrival =
            first_sample_time
            + uhd::time_spec_t(
                  0,
                  static_cast<double>(det.peak_index)
                      / cfg.sample_rate);

        const double delay_us =
            (arrival - tx_time).get_real_secs() * 1e6;

        delays_us.push_back(delay_us);

        std::cout
            << "  iteration "
            << std::setw(2) << i
            << ": peak at sample "
            << std::setw(5) << det.peak_index
            << ", peak/mean = "
            << std::fixed << std::setprecision(1)
            << std::setw(6) << det.peak_to_mean
            << ", TX -> RX = "
            << std::setprecision(2)
            << delay_us
            << " us\n";
    }

    if (delays_us.empty())
    {
        std::cout
            << "\nNo successful detections.\n"
            << "With the ports terminated rather than looped back,\n"
            << "TX may simply not reach RX -- check the cabling.\n";

        return;
    }

    const auto stats =
        calculate_stats(delays_us);

    std::cout
        << "\nTX -> RX delay from ZC correlation ("
        << delays_us.size() << "/" << iterations
        << " detected):\n";

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "  min    = " << stats.min << " us\n";
    std::cout << "  median = " << stats.median << " us\n";
    std::cout << "  mean   = " << stats.mean << " us\n";
    std::cout << "  max    = " << stats.max << " us\n";

    std::cout
        << "\nInterpretation:\n"
        << "The correlation peak marks the arrival of the burst\n"
        << "itself, so this delay is a real loopback measurement --\n"
        << "unlike the value in test 4, which only compares the RX\n"
        << "window's start time against the scheduled TX time.\n";
}


// ============================================================
// Segmented (CFO-tolerant) Zadoff-Chu detection
// ============================================================

/*
 * Two B210s on their own internal TCXOs can sit up to a few ppm
 * apart. At 3.5 GHz that is several kHz of carrier offset, and a
 * coherent correlation across a whole 401-sample sequence rotates
 * through multiple phase cycles and smears the peak away.
 *
 * Splitting the correlation into short segments keeps each one
 * inside a small fraction of a cycle. The segments are combined
 * by magnitude, which throws away the phase between them and so
 * no longer cares about the offset. The cost is a few dB of
 * processing gain versus the coherent version.
 */

struct ZcSegmentedDetection
{
    bool found = false;

    size_t peak_index = 0;

    double peak_magnitude = 0.0;
    double mean_magnitude = 0.0;
    double peak_to_mean = 0.0;

    /*
     * Carrier offset estimated from the phase advance between
     * consecutive segment correlations at the peak lag.
     */
    double cfo_hz = 0.0;
};


ZcSegmentedDetection detect_zadoff_chu_segmented(
    const std::vector<complex_t>& capture,
    const std::vector<complex_t>& reference,
    double sample_rate,
    size_t num_segments = 16,
    double threshold = 6.0)
{
    ZcSegmentedDetection result{};

    if (reference.empty()
        || num_segments == 0
        || capture.size() < reference.size())
    {
        return result;
    }

    const size_t seg_len =
        reference.size() / num_segments;

    if (seg_len == 0)
        return result;

    const size_t num_lags =
        capture.size() - reference.size() + 1;

    double sum_magnitude = 0.0;

    std::vector<complex_t> best_segments;

    for (size_t lag = 0; lag < num_lags; ++lag)
    {
        double magnitude = 0.0;

        std::vector<complex_t> segments;
        segments.reserve(num_segments);

        for (size_t s = 0; s < num_segments; ++s)
        {
            complex_t acc(0.0f, 0.0f);

            const size_t base = s * seg_len;

            for (size_t k = 0; k < seg_len; ++k)
            {
                acc += capture[lag + base + k]
                     * std::conj(reference[base + k]);
            }

            /*
             * Non-coherent combining: sum the magnitudes, not the
             * complex values, so a phase ramp across segments does
             * not cancel the result.
             */
            magnitude += std::abs(acc);

            segments.push_back(acc);
        }

        sum_magnitude += magnitude;

        if (magnitude > result.peak_magnitude)
        {
            result.peak_magnitude = magnitude;
            result.peak_index = lag;
            best_segments = segments;
        }
    }

    result.mean_magnitude =
        sum_magnitude / static_cast<double>(num_lags);

    if (result.mean_magnitude > 0.0)
    {
        result.peak_to_mean =
            result.peak_magnitude / result.mean_magnitude;
    }

    result.found =
        result.peak_to_mean >= threshold;

    /*
     * Each segment spans seg_len/sample_rate seconds. A carrier
     * offset shows up as a constant phase step from one segment
     * correlation to the next.
     */
    if (result.found && best_segments.size() >= 2)
    {
        complex_t phase_step(0.0f, 0.0f);

        for (size_t s = 1; s < best_segments.size(); ++s)
        {
            phase_step +=
                best_segments[s]
                * std::conj(best_segments[s - 1]);
        }

        const double seg_seconds =
            static_cast<double>(seg_len) / sample_rate;

        result.cfo_hz =
            std::arg(phase_step)
            / (2.0 * M_PI * seg_seconds);
    }

    return result;
}


// ============================================================
// Self-test for the segmented detector
// ============================================================

/*
 * The coherent detector has zc_self_test(); this covers the
 * segmented one on the same terms -- plant the sequence at a known
 * offset, add noise, and check each segment count recovers it.
 */
void zc_segmented_self_test(double sample_rate)
{
    std::cout << "\nSegmented detector self-test:\n";

    const size_t zc_length = 401;
    const auto zc = make_zadoff_chu(zc_length, 25);

    const size_t planted = 777;

    std::vector<complex_t> capture(4080, complex_t(0.0f, 0.0f));

    /*
     * Weak signal plus noise, roughly the level seen on the cable.
     */
    std::srand(1);

    for (auto& c : capture)
    {
        c = complex_t(
            0.001f * (float(std::rand()) / RAND_MAX - 0.5f),
            0.001f * (float(std::rand()) / RAND_MAX - 0.5f));
    }

    for (size_t k = 0; k < zc_length; ++k)
        capture[planted + k] += 0.004f * zc[k];

    for (size_t nseg : {1u, 2u, 4u, 8u, 16u})
    {
        const auto d =
            detect_zadoff_chu_segmented(
                capture, zc, sample_rate, nseg, 4.0);

        std::cout
            << "  segments = " << std::setw(2) << nseg
            << " : peak/mean = "
            << std::fixed << std::setprecision(2)
            << std::setw(7) << d.peak_to_mean
            << ", offset = " << std::setw(5) << d.peak_index
            << (d.peak_index == planted ? "  (correct)" : "  (WRONG)")
            << "\n";
    }
}


// ============================================================
// Dump one live capture window for offline comparison
// ============================================================

void zc_dump_window(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double rx_gain)
{
    usrp->set_rx_gain(rx_gain);

    std::cout
        << "\nDumping a window captured through the same"
        << " continuous-stream loop as zcrx.\n"
        << "RX gain = " << usrp->get_rx_gain() << " dB\n";

    const size_t zc_length = 401;
    const auto zc = make_zadoff_chu(zc_length, 25);

    auto rx_stream =
        usrp->get_rx_stream(uhd::stream_args_t("fc32", "sc16"));

    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    cmd.stream_now = true;
    rx_stream->issue_stream_cmd(cmd);

    std::vector<complex_t> chunk(rx_stream->get_max_num_samps());
    std::vector<complex_t> window;

    const size_t window_len = zc_length * 8;

    size_t recv_calls = 0;
    size_t overflows = 0;

    while (window.size() < window_len)
    {
        uhd::rx_metadata_t md;

        const size_t got =
            rx_stream->recv(chunk.data(), chunk.size(), md, 1.0);

        ++recv_calls;

        if (md.error_code
            == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            ++overflows;
            continue;
        }

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
            continue;

        window.insert(
            window.end(), chunk.begin(), chunk.begin() + got);
    }

    cmd.stream_mode =
        uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

    rx_stream->issue_stream_cmd(cmd);

    double sum_sq = 0.0;

    for (const auto& c : window)
        sum_sq += std::norm(c);

    std::cout
        << "  samples   = " << window.size() << "\n"
        << "  recvcalls = " << recv_calls << "\n"
        << "  overflows = " << overflows << "\n"
        << "  rms       = "
        << std::sqrt(sum_sq / double(window.size())) << "\n";

    for (size_t nseg : {1u, 4u, 16u})
    {
        const auto d =
            detect_zadoff_chu_segmented(
                window, zc, cfg.sample_rate, nseg, 4.0);

        std::cout
            << "  C++ detect nseg=" << std::setw(2) << nseg
            << " : peak/mean = " << std::fixed
            << std::setprecision(2) << d.peak_to_mean
            << ", offset = " << d.peak_index << "\n";
    }

    std::ofstream f("/tmp/zc_window.f32", std::ios::binary);

    f.write(
        reinterpret_cast<const char*>(window.data()),
        std::streamsize(window.size() * sizeof(complex_t)));

    std::cout << "  wrote /tmp/zc_window.f32\n";
}


// ============================================================
// 9a. Two-host ZC transmitter
// ============================================================

void transmit_zadoff_chu(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double seconds)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "9a. ZADOFF-CHU TRANSMITTER (two-host)\n";
    std::cout << "====================================================\n";

    const size_t zc_length = 401;
    const size_t zc_root = 25;

    const auto zc =
        make_zadoff_chu(zc_length, zc_root);

    /*
     * Pad each burst with silence so the receiver sees a clear
     * gap between repetitions and cannot confuse two of them.
     */
    std::vector<complex_t> burst = zc;

    burst.resize(zc_length * 4, complex_t(0.0f, 0.0f));

    /*
     * Batch several repetitions into one send.
     *
     * Feeding the device one 1604-sample burst per call does not
     * keep the TX chain fed: it underflows, then reports sequence
     * errors, and the radio goes silent while send() still accepts
     * samples at the nominal rate. A buffer of a few tens of
     * thousands of samples streams cleanly.
     *
     * Scale off full scale as well -- Zadoff-Chu is constant
     * modulus at 1.0, which leaves no headroom in the converter.
     */
    const size_t reps = 10;

    std::vector<complex_t> buffer;
    buffer.reserve(burst.size() * reps);

    for (size_t r = 0; r < reps; ++r)
    {
        for (const auto& c : burst)
            buffer.push_back(c * 0.7f);
    }

    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    std::cout
        << "Transmitting ZC (length " << zc_length
        << ", root " << zc_root << ") for "
        << seconds << " s...\n";

    const auto t_start = clock_type::now();

    size_t bursts = 0;
    size_t short_sends = 0;

    /*
     * Each repetition is sent as a self-contained burst.
     *
     * Marking only the first packet as start-of-burst and never
     * ending it makes the whole run one infinite burst: the first
     * underflow then breaks the device's burst state and every
     * later packet is rejected with a sequence error, so the radio
     * goes quiet while send() still happily accepts samples.
     * Independent bursts recover from an underflow on their own.
     */
    uhd::tx_metadata_t md;

    md.start_of_burst = true;
    md.end_of_burst = false;
    md.has_time_spec = false;

    while (std::chrono::duration<double>(
               clock_type::now() - t_start).count() < seconds)
    {
        const size_t sent =
            tx_stream->send(
                buffer.data(),
                buffer.size(),
                md,
                1.0);

        if (sent != buffer.size())
            ++short_sends;

        md.start_of_burst = false;

        bursts += reps;
    }

    md.end_of_burst = true;
    tx_stream->send(buffer.data(), 0, md);

    std::cout
        << "Sent " << bursts << " bursts ("
        << bursts * burst.size() << " samples)";

    if (short_sends)
        std::cout << ", " << short_sends << " short sends";

    std::cout << ".\n";
}


// ============================================================
// 9b. Two-host ZC receiver
// ============================================================

void receive_zadoff_chu(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double seconds,
    size_t num_segments = 4,
    double threshold = 4.0,
    double rx_gain = 50.0)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "9b. ZADOFF-CHU RECEIVER (two-host)\n";
    std::cout << "====================================================\n";

    /*
     * The 30 dB default leaves the signal only a few dB above the
     * 12-bit ADC quantisation floor. A CW tone still shows up
     * because an FFT gives ~45 dB of processing gain, but a 401
     * sample correlation only gives ~26 dB and loses it. Raise the
     * gain so the correlator has something to work with.
     */
    usrp->set_rx_gain(rx_gain);

    std::cout
        << "RX gain set to "
        << usrp->get_rx_gain()
        << " dB\n";

    const size_t zc_length = 401;
    const size_t zc_root = 25;

    const auto zc =
        make_zadoff_chu(zc_length, zc_root);

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    cmd.stream_now = true;

    rx_stream->issue_stream_cmd(cmd);

    std::vector<complex_t> chunk(
        rx_stream->get_max_num_samps());

    /*
     * Correlate over a window a few bursts long, carrying the tail
     * of the previous window so a sequence straddling the boundary
     * is still seen whole.
     */
    const size_t window_len = zc_length * 8;

    std::vector<complex_t> window;
    window.reserve(window_len * 2);

    const auto t_start = clock_type::now();

    size_t num_windows = 0;
    size_t num_detected = 0;
    size_t num_overflow = 0;

    std::vector<double> ratios;
    std::vector<double> cfos;

    std::cout
        << "Listening for "
        << seconds
        << " s (detector: " << num_segments
        << " segments, threshold " << threshold << ")...\n\n";

    while (std::chrono::duration<double>(
               clock_type::now() - t_start).count() < seconds)
    {
        uhd::rx_metadata_t md;

        const size_t got =
            rx_stream->recv(
                chunk.data(),
                chunk.size(),
                md,
                1.0);

        if (md.error_code
            == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            ++num_overflow;
            continue;
        }

        if (md.error_code
            != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            std::cout
                << "  RX error: "
                << rx_error_to_string(md.error_code)
                << "\n";

            continue;
        }

        window.insert(
            window.end(),
            chunk.begin(),
            chunk.begin() + got);

        if (window.size() < window_len)
            continue;

        const auto det =
            detect_zadoff_chu_segmented(
                window,
                zc,
                cfg.sample_rate,
                num_segments,
                threshold);

        ++num_windows;

        if (num_windows <= 15)
        {
            double sq = 0.0;

            for (const auto& c : window)
                sq += std::norm(c);

            std::cout
                << "  [win " << std::setw(3) << num_windows
                << "] size=" << std::setw(5) << window.size()
                << " rms=" << std::fixed << std::setprecision(5)
                << std::sqrt(sq / double(window.size()))
                << " peak/mean=" << std::setprecision(2)
                << det.peak_to_mean
                << " offset=" << det.peak_index
                << "\n";
        }

        ratios.push_back(det.peak_to_mean);

        if (det.found)
        {
            ++num_detected;

            cfos.push_back(det.cfo_hz);

            if (num_detected <= 10)
            {
                std::cout
                    << "  DETECT: offset "
                    << std::setw(5) << det.peak_index
                    << ", peak/mean = "
                    << std::fixed << std::setprecision(1)
                    << std::setw(6) << det.peak_to_mean
                    << ", CFO = "
                    << std::setprecision(0) << std::setw(7)
                    << det.cfo_hz
                    << " Hz\n";
            }
        }

        /*
         * Keep one sequence length of history for the next window.
         */
        window.erase(
            window.begin(),
            window.end() - static_cast<long>(zc_length));
    }

    cmd.stream_mode =
        uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

    rx_stream->issue_stream_cmd(cmd);

    const auto ratio_stats = calculate_stats(ratios);

    std::cout
        << "\nResult over " << num_windows << " windows:\n";

    std::cout
        << "  detected      = " << num_detected
        << " (" << std::fixed << std::setprecision(1)
        << (num_windows
                ? 100.0 * double(num_detected) / double(num_windows)
                : 0.0)
        << " %)\n";

    std::cout << "  overflows     = " << num_overflow << "\n";

    std::cout
        << std::setprecision(2)
        << "  peak/mean min = " << ratio_stats.min << "\n"
        << "  peak/mean med = " << ratio_stats.median << "\n"
        << "  peak/mean max = " << ratio_stats.max << "\n";

    if (!cfos.empty())
    {
        const auto cfo_stats = calculate_stats(cfos);

        std::cout
            << std::setprecision(0)
            << "  CFO median    = " << cfo_stats.median << " Hz"
            << "  (" << std::setprecision(2)
            << cfo_stats.median / cfg.frequency * 1e6
            << " ppm)\n";
    }

    std::cout
        << "\nInterpretation:\n";

    if (num_detected == 0)
    {
        std::cout
            << "Nothing detected. Either the transmitter was not\n"
            << "running during this window, the cable/attenuation\n"
            << "is wrong, or the two radios are not on the same\n"
            << "frequency.\n";
    }
    else
    {
        std::cout
            << "The peak marks where the sequence sits in this\n"
            << "capture. Because the two hosts run on independent\n"
            << "internal clocks, the offset is only meaningful\n"
            << "within a capture -- it is NOT an absolute TX -> RX\n"
            << "latency. The CFO figure is the carrier offset\n"
            << "between the two TCXOs.\n";
    }
}


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
// Complete experiment
// ============================================================

void run_all(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    measure_send_execution(
        usrp,
        cfg);

    find_minimum_safe_lead(
        usrp,
        cfg);

    measure_tx_rx_timestamp_latency(
        usrp,
        cfg);

    measure_host_recv_time(
        usrp,
        cfg);

    monitor_tx_metadata(
        usrp,
        cfg);

    monitor_rx_metadata(
        usrp,
        cfg);

    run_zadoff_chu_test(
        usrp,
        cfg);
}


// ============================================================
// Main
// ============================================================

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    Config cfg;

    std::string mode = "all";

    if (argc >= 2)
        mode = argv[1];

    std::cout
        << "===============================================\n"
        << " UHD B210 TX/RX LATENCY EXPERIMENT\n"
        << "===============================================\n";

    std::cout
        << "Mode: "
        << mode
        << "\n";

    auto usrp =
        create_usrp(cfg);

    if (mode == "send")
    {
        measure_send_execution(
            usrp,
            cfg);
    }
    else if (mode == "lead")
    {
        find_minimum_safe_lead(
            usrp,
            cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? static_cast<size_t>(std::stoul(argv[3])) : 50);
    }
    else if (mode == "txrx")
    {
        measure_tx_rx_timestamp_latency(
            usrp,
            cfg);
    }
    else if (mode == "recv")
    {
        measure_host_recv_time(
            usrp,
            cfg);
    }
    else if (mode == "txmeta")
    {
        monitor_tx_metadata(
            usrp,
            cfg);
    }
    else if (mode == "rxmeta")
    {
        monitor_rx_metadata(
            usrp,
            cfg);
    }
    else if (mode == "zc")
    {
        run_zadoff_chu_test(
            usrp,
            cfg);
    }
    else if (mode == "zctx")
    {
        /*
         * The default 20 dB is far too low to push a detectable
         * signal through the inter-host cable and its attenuator,
         * so allow an override here.
         */
        if (argc >= 4)
        {
            const double g = std::stod(argv[3]);

            usrp->set_tx_gain(g);

            std::cout
                << "TX gain overridden to "
                << usrp->get_tx_gain()
                << " dB\n";
        }

        transmit_zadoff_chu(
            usrp,
            cfg,
            argc >= 3 ? std::stod(argv[2]) : 10.0);
    }
    else if (mode == "zcdump")
    {
        zc_dump_window(
            usrp, cfg, argc >= 3 ? std::stod(argv[2]) : 50.0);
    }
    else if (mode == "zcseg")
    {
        zc_segmented_self_test(cfg.sample_rate);
    }
    else if (mode == "zcrx")
    {
        receive_zadoff_chu(
            usrp,
            cfg,
            argc >= 3 ? std::stod(argv[2]) : 10.0,
            argc >= 4 ? static_cast<size_t>(std::stoul(argv[3])) : 4,
            argc >= 5 ? std::stod(argv[4]) : 4.0,
            argc >= 6 ? std::stod(argv[5]) : 50.0);
    }
    else if (mode == "tdd")
    {
        run_tdd_slots(
            usrp,
            cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? static_cast<size_t>(std::stoul(argv[3])) : 1000,
            argc >= 5 ? std::stod(argv[4]) : 20.0);
    }
    else if (mode == "tdds")
    {
        run_tdd_stream(
            usrp, cfg,
            argc >= 3 ? std::stod(argv[2]) : 1.0,
            argc >= 4 ? std::stod(argv[3]) : 5.0,
            argc >= 5 ? std::stod(argv[4]) : 0.5);
    }
    else if (mode == "all")
    {
        run_all(
            usrp,
            cfg);
    }
    else
    {
        std::cerr
            << "Unknown mode: "
            << mode
            << "\n\n";

        std::cerr
            << "Usage:\n"
            << "  ./latency_test send\n"
            << "  ./latency_test lead [slot_ms] [iterations]\n"
            << "  ./latency_test tdd [slot_ms] [slots] [pipeline_ms]\n"
            << "  ./latency_test tdds [slot_ms] [seconds] [duty]\n"
            << "  ./latency_test txrx\n"
            << "  ./latency_test recv\n"
            << "  ./latency_test txmeta\n"
            << "  ./latency_test rxmeta\n"
            << "  ./latency_test zc\n"
            << "  ./latency_test zctx [seconds] [tx_gain]  (two-host: sender)\n"
            << "  ./latency_test zcrx [seconds] [segments] [threshold] [rx_gain]\n"
            << "  ./latency_test zcseg            (offline detector self-test)\n"
            << "  ./latency_test zcdump [rx_gain] (dump one live window)\n"
            << "  ./latency_test all\n";

        return 1;
    }

    std::cout
        << "\nExperiment complete.\n";

    return 0;
}