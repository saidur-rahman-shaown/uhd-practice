#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>

#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <vector>

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // ------------------------------------------------------------
    // 1. Create USRP
    // ------------------------------------------------------------

    auto usrp = uhd::usrp::multi_usrp::make("");


    // ------------------------------------------------------------
    // 2. TX configuration
    // ------------------------------------------------------------

    const double tx_frequency = 915e6;
    const double tx_sample_rate = 1e6;
    const double tx_gain = 20.0;

    const std::size_t channel = 0;


    // ------------------------------------------------------------
    // 3. Configure TX
    // ------------------------------------------------------------

    usrp->set_tx_rate(tx_sample_rate, channel);
    usrp->set_tx_freq(tx_frequency, channel);
    usrp->set_tx_gain(tx_gain, channel);


    // ------------------------------------------------------------
    // 4. Print configuration
    // ------------------------------------------------------------

    std::cout << "TX frequency: "
              << usrp->get_tx_freq(channel)
              << " Hz\n";

    std::cout << "TX sample rate: "
              << usrp->get_tx_rate(channel)
              << " S/s\n";

    std::cout << "TX gain: "
              << usrp->get_tx_gain(channel)
              << " dB\n";


    // ------------------------------------------------------------
    // 5. Create TX streamer
    // ------------------------------------------------------------

    uhd::stream_args_t stream_args("fc32", "sc16");

    auto tx_stream =
        usrp->get_tx_stream(stream_args);


    // ------------------------------------------------------------
    // 6. Generate a known IQ waveform
    // ------------------------------------------------------------

    const std::size_t samples_to_send = 100000;

    std::vector<std::complex<float>> buffer(
        samples_to_send
    );

    const double tone_frequency = 100e3;

    const double sample_period =
        1.0 / tx_sample_rate;

    const double pi =
        3.14159265358979323846;

    for (std::size_t n = 0;
         n < samples_to_send;
         ++n)
    {
        const double t =
            static_cast<double>(n) * sample_period;

        const double phase =
            2.0 * pi * tone_frequency * t;

        buffer[n] =
            std::complex<float>(
                std::cos(phase),
                std::sin(phase)
            );
    }


    // ------------------------------------------------------------
    // 7. Get current USRP device time
    // ------------------------------------------------------------

    const double current_device_time =
        usrp->get_time_now().get_real_secs();

    const double tx_start_time =
        current_device_time + 1.0;


    std::cout << std::fixed
              << std::setprecision(9);

    std::cout << "\nCurrent device time: "
              << current_device_time
              << " seconds\n";

    std::cout << "Scheduled TX start time: "
              << tx_start_time
              << " seconds\n";


    // ------------------------------------------------------------
    // 8. Create TX metadata
    // ------------------------------------------------------------

    uhd::tx_metadata_t metadata;

    metadata.start_of_burst = true;
    metadata.end_of_burst = true;

    metadata.has_time_spec = true;

    metadata.time_spec =
        uhd::time_spec_t(tx_start_time);


    // ------------------------------------------------------------
    // 9. Send the waveform
    // ------------------------------------------------------------

    std::cout << "\nSending timed TX burst...\n";

    const std::size_t samples_sent =
        tx_stream->send(
            buffer.data(),
            buffer.size(),
            metadata
        );


    // ------------------------------------------------------------
    // 10. Print result
    // ------------------------------------------------------------

    std::cout << "\nTX send() returned: "
              << samples_sent
              << " samples\n";

    std::cout << "Scheduled TX time: "
              << tx_start_time
              << " seconds\n";


    return 0;
}
