#include <uhd/usrp/multi_usrp.hpp>

#include <iostream>

int main()
{
    std::cout << "Creating USRP device..." << std::endl;

    auto usrp = uhd::usrp::multi_usrp::make("");

    std::cout << "USRP created successfully!" << std::endl;

    // Configure RX
    const double rx_frequency = 915e6;
    const double rx_sample_rate = 1e6;
    const double rx_gain = 30.0;

    usrp->set_rx_freq(rx_frequency);
    usrp->set_rx_rate(rx_sample_rate);
    usrp->set_rx_gain(rx_gain);

    // Read back configuration
    std::cout << "\nRX configuration:" << std::endl;

    std::cout << "Frequency: "
              << usrp->get_rx_freq()
              << " Hz" << std::endl;

    std::cout << "Sample rate: "
              << usrp->get_rx_rate()
              << " S/s" << std::endl;

    std::cout << "Gain: "
              << usrp->get_rx_gain()
              << " dB" << std::endl;

    return 0;
}
