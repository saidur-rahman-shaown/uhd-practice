#pragma once

// Shared setup for the experiments in this directory: the knobs, the device
// bring-up and the test waveform. Kept in one place so latency_test and
// tdd_latency_test cannot drift apart on frequency, rate or gain.

#include <uhd/usrp/multi_usrp.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
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
