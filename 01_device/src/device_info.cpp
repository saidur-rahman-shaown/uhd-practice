#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <iostream>

/*
 * What is attached, and what can it do?
 *
 * The first thing to run on a new machine: it proves UHD can find and
 * open the radio before any experiment blames itself for a problem
 * that is really a USB cable.
 */

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    auto usrp = uhd::usrp::multi_usrp::make(std::string(""));

    std::cout << "\n" << usrp->get_pp_string() << "\n";

    std::cout
        << "Master clock : " << usrp->get_master_clock_rate() / 1e6 << " MHz\n"
        << "TX antennas  : ";
    for (const auto& a : usrp->get_tx_antennas()) std::cout << a << " ";

    std::cout << "\nRX antennas  : ";
    for (const auto& a : usrp->get_rx_antennas()) std::cout << a << " ";

    const auto tx_r = usrp->get_tx_freq_range();
    const auto rx_r = usrp->get_rx_freq_range();
    const auto tx_g = usrp->get_tx_gain_range();
    const auto rx_g = usrp->get_rx_gain_range();

    std::cout
        << "\nTX freq      : " << tx_r.start() / 1e6 << " - "
        << tx_r.stop() / 1e6 << " MHz\n"
        << "RX freq      : " << rx_r.start() / 1e6 << " - "
        << rx_r.stop() / 1e6 << " MHz\n"
        << "TX gain      : " << tx_g.start() << " - " << tx_g.stop() << " dB\n"
        << "RX gain      : " << rx_g.start() << " - " << rx_g.stop() << " dB\n"
        << "Clock srcs   : ";
    for (const auto& c : usrp->get_clock_sources(0)) std::cout << c << " ";

    std::cout << "\nTime srcs    : ";
    for (const auto& t : usrp->get_time_sources(0)) std::cout << t << " ";

    std::cout << "\n\nSensors: ";
    for (const auto& s : usrp->get_tx_sensor_names(0))
        std::cout << "tx/" << s << " ";
    for (const auto& s : usrp->get_mboard_sensor_names(0))
        std::cout << "mb/" << s << " ";

    std::cout << "\n";

    return 0;
}
