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
// 2 + 3. Timed TX and minimum safe lead time
// ============================================================

bool test_tx_lead_time(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg,
    double lead_time)
{
    auto tx_stream =
        usrp->get_tx_stream(
            uhd::stream_args_t("fc32", "sc16"));

    auto samples =
        make_waveform(
            cfg.num_samples,
            cfg.sample_rate,
            10e3);

    /*
     * Read the USRP's device clock.
     */
    const uhd::time_spec_t now =
        usrp->get_time_now();

    /*
     * Schedule TX in the future.
     */
    const uhd::time_spec_t tx_time =
        now + lead_time;

    /*
     * Call send() immediately.
     */
    bool success =
        send_timed_burst(
            tx_stream,
            samples,
            tx_time);

    /*
     * Wait long enough for the device to process it.
     */
    std::this_thread::sleep_for(
        std::chrono::duration<double>(
            lead_time + 0.05));

    return success;
}


void find_minimum_safe_lead(
    uhd::usrp::multi_usrp::sptr usrp,
    const Config& cfg)
{
    std::cout << "\n";
    std::cout << "====================================================\n";
    std::cout << "2 + 3. TIMED TX / MINIMUM SAFE LEAD TIME\n";
    std::cout << "====================================================\n";

    std::vector<double> lead_times_ms =
    {
        0.1,
        0.2,
        0.5,
        1.0,
        2.0,
        5.0,
        10.0,
        20.0
    };

    std::cout
        << "\nLead time       Result\n";
    std::cout
        << "-------------------------\n";

    for (double lead_ms : lead_times_ms)
    {
        /*
         * Flush any previous TX state.
         */
        usrp->clear_command_time();

        bool success =
            test_tx_lead_time(
                usrp,
                cfg,
                lead_ms / 1000.0);

        std::cout
            << std::setw(8)
            << lead_ms
            << " ms       "
            << (success ? "send accepted" : "FAILED")
            << "\n";
    }

    std::cout << "\n";
    std::cout
        << "NOTE:\n"
        << "send() returning successfully does NOT prove that\n"
        << "the TX was transmitted on time.\n"
        << "For true late-TX detection, monitor UHD async TX\n"
        << "metadata in the next refinement of this experiment.\n";
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
            cfg);
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
            << "  ./latency_test lead\n"
            << "  ./latency_test txrx\n"
            << "  ./latency_test recv\n"
            << "  ./latency_test txmeta\n"
            << "  ./latency_test rxmeta\n"
            << "  ./latency_test all\n";

        return 1;
    }

    std::cout
        << "\nExperiment complete.\n";

    return 0;
}