#pragma once

// Shared setup for the experiments in this directory: the knobs, the device
// bring-up and the test waveform. Kept in one place so latency_test and
// tdd_latency_test cannot drift apart on frequency, rate or gain.

#include <uhd/usrp/multi_usrp.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <cstdlib>
#include <string>
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
// Over-the-wire sample format
// ============================================================

/*
 * How many bytes each complex sample costs on the USB link.
 *
 * sc12 is the default because it is free on this hardware: the
 * B210's AD9361 has 12-bit converters, so sc12 carries every bit the
 * radio produces and sc16 merely pads each sample with zeros. The
 * padding is not harmless at high rates -- 30.72 MS/s in both
 * directions is about 246 MB/s under sc16 against roughly 184 under
 * sc12, and the first figure is close enough to the usable USB 3
 * ceiling to underflow. Measured over sixty-second runs, sc16 passed
 * 2 of 3 and sc12 passed 9 of 9.
 *
 * Override for comparisons:  NR_OTW=sc16 ./tdd_latency_test ...
 */

inline std::string otw_format()
{
    const char* v = std::getenv("NR_OTW");

    return v ? std::string(v) : std::string("sc12");
}


// ============================================================
// Master clock, so a requested rate is actually achievable
// ============================================================

/*
 * The B210 makes its sample rate by dividing the master clock, which
 * defaults to 32 MHz. Asking for a rate that is not a divisor of it
 * -- every NR rate, as it happens -- silently yields something else:
 * 30.72 MS/s comes back as 32, and 23.04 as 16. The request appears
 * to succeed and the wrong rate is then measured as though it were
 * the one asked for.
 *
 * Driving the master clock at the sample rate itself gives a divisor
 * of one, which is exact.
 */

inline void set_master_clock_for(
    uhd::usrp::multi_usrp::sptr usrp,
    double rate)
{
    if (rate <= 0.0) return;

    if (std::fabs(usrp->get_master_clock_rate() - rate) <= 1.0) return;

    try
    {
        usrp->set_master_clock_rate(rate);
    }
    catch (const std::exception& e)
    {
        std::cout
            << "  NOTE: master clock " << rate / 1e6
            << " MHz rejected (" << e.what() << ")\n";
    }
}


// ============================================================
// How much to hand the device per send()
// ============================================================

/*
 * Buffer depth has to be measured in time, not in samples. A fixed
 * sample count means a buffer that shrinks as the rate rises: 16k
 * samples is 16 ms at 1 MS/s but only half a millisecond at
 * 30.72 MS/s, which is nowhere near enough to ride out host jitter.
 * Sizing the same buffer by time instead took one 30.72 MS/s run
 * from 74 underflows to none.
 *
 * Ten milliseconds is comfortable at every rate used here.
 */

inline size_t send_buffer_samples(double sample_rate)
{
    return static_cast<size_t>(sample_rate * 0.010);
}
