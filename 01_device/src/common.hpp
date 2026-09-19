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


