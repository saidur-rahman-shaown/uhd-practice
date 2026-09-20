#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

/*
 * Zadoff-Chu: generation, detection, and two-host synchronisation.
 *
 * The sequence is constant modulus and its periodic autocorrelation is
 * an impulse, so correlating a capture against a known one gives a
 * single sharp peak at the delay. That makes it the natural tool for
 * finding where a burst actually landed -- which is what the timed-TX
 * experiment in 05_timed_commands could not do by reading timestamps.
 *
 * Start with `zcseg`, which needs no radio.
 */

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
            uhd::stream_args_t("fc32", otw_format()));

    auto rx_stream =
        usrp->get_rx_stream(
            uhd::stream_args_t("fc32", otw_format()));

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
        usrp->get_rx_stream(uhd::stream_args_t("fc32", otw_format()));

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
            uhd::stream_args_t("fc32", otw_format()));

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
            uhd::stream_args_t("fc32", otw_format()));

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
// Main
// ============================================================

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    Config cfg;

    const std::string mode = argc >= 2 ? argv[1] : "zcseg";

    std::cout
        << "===============================================\n"
        << " ZADOFF-CHU GENERATION, DETECTION AND SYNC\n"
        << "===============================================\n"
        << "Mode: " << mode << "\n";

    /*
     * The offline self-tests touch no hardware, so they run without a
     * radio attached -- useful for checking the correlator on any machine.
     */
    if (mode == "zcseg")
    {
        zc_segmented_self_test(cfg.sample_rate);
        return 0;
    }

    auto usrp = create_usrp(cfg);

    if (mode == "single")
    {
        run_zadoff_chu_test(usrp, cfg);
    }
    else if (mode == "tx")
    {
        if (argc >= 4)
        {
            usrp->set_tx_gain(std::stod(argv[3]));

            std::cout
                << "TX gain overridden to "
                << usrp->get_tx_gain() << " dB\n";
        }

        transmit_zadoff_chu(
            usrp, cfg, argc >= 3 ? std::stod(argv[2]) : 10.0);
    }
    else if (mode == "rx")
    {
        receive_zadoff_chu(
            usrp, cfg,
            argc >= 3 ? std::stod(argv[2]) : 10.0,
            argc >= 4 ? static_cast<size_t>(std::stoul(argv[3])) : 4,
            argc >= 5 ? std::stod(argv[4]) : 4.0,
            argc >= 6 ? std::stod(argv[5]) : 50.0);
    }
    else if (mode == "dump")
    {
        zc_dump_window(usrp, cfg, argc >= 3 ? std::stod(argv[2]) : 50.0);
    }
    else
    {
        std::cerr
            << "Unknown mode: " << mode << "\n\n"
            << "Usage:\n"
            << "  ./zc_sync zcseg                       offline detector self-test\n"
            << "  ./zc_sync single                      TX and RX on one radio\n"
            << "  ./zc_sync tx   [seconds] [tx_gain]    two-host sender\n"
            << "  ./zc_sync rx   [seconds] [segments] [threshold] [rx_gain]\n"
            << "  ./zc_sync dump [rx_gain]              save one live window\n";

        return 1;
    }

    std::cout << "\nDone.\n";

    return 0;
}
