#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>

#include <algorithm>
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
    // 2. RX configuration
    // ------------------------------------------------------------
    const double rx_frequency = 915e6;
    const double rx_sample_rate = 1e6;
    const double rx_gain = 30.0;
    const std::size_t channel = 0;

    usrp->set_rx_rate(rx_sample_rate, channel);
    usrp->set_rx_freq(rx_frequency, channel);
    usrp->set_rx_gain(rx_gain, channel);

    std::cout << "RX frequency: "
              << usrp->get_rx_freq(channel) << " Hz\n";

    std::cout << "RX sample rate: "
              << usrp->get_rx_rate(channel) << " S/s\n";

    std::cout << "RX gain: "
              << usrp->get_rx_gain(channel) << " dB\n";

    // ------------------------------------------------------------
    // 3. Create RX streamer
    // ------------------------------------------------------------
    uhd::stream_args_t stream_args("fc32", "sc16");

    auto rx_stream = usrp->get_rx_stream(stream_args);

    // ------------------------------------------------------------
    // 4. Buffer
    // ------------------------------------------------------------
    const std::size_t samples_to_receive = 1'000'000;

    std::vector<std::complex<float>> buffer(samples_to_receive);

    // ------------------------------------------------------------
    // 5. Determine when RX should start
    // ------------------------------------------------------------
    const double current_device_time =
        usrp->get_time_now().get_real_secs();

    const double rx_start_time =
        current_device_time + 0.5;

    std::cout << std::fixed << std::setprecision(9);

    std::cout << "\nCurrent device time: "
              << current_device_time
              << " seconds\n";

    std::cout << "Scheduled RX start time: "
              << rx_start_time
              << " seconds\n";

    // ------------------------------------------------------------
    // 6. Schedule RX
    // ------------------------------------------------------------
    uhd::stream_cmd_t stream_cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
    );

    stream_cmd.stream_now = false;
    stream_cmd.time_spec = uhd::time_spec_t(rx_start_time);

    rx_stream->issue_stream_cmd(stream_cmd);

    std::cout << "\nStarting timed RX streaming...\n";

    // ------------------------------------------------------------
    // 7. Receive samples
    // ------------------------------------------------------------
    uhd::rx_metadata_t metadata;

    const std::size_t samples_received =
        rx_stream->recv(
            buffer.data(),
            buffer.size(),
            metadata,
            1.0
        );

    // ------------------------------------------------------------
    // 8. Print metadata
    // ------------------------------------------------------------
    std::cout << "\nMetadata:\n";

    std::cout << "error_code: "
              << metadata.error_code
              << '\n';

    std::cout << "has_time_spec: "
              << metadata.has_time_spec
              << '\n';

    double first_sample_time = 0.0;

    if (metadata.has_time_spec)
    {
        first_sample_time =
            metadata.time_spec.get_real_secs();

        std::cout << "time_spec: "
                  << first_sample_time
                  << " seconds\n";
    }

    std::cout << "start_of_burst: "
              << metadata.start_of_burst
              << '\n';

    std::cout << "end_of_burst: "
              << metadata.end_of_burst
              << '\n';

    std::cout << "more_fragments: "
              << metadata.more_fragments
              << '\n';

    std::cout << "fragment_offset: "
              << metadata.fragment_offset
              << '\n';

    std::cout << "\nrecv() returned "
              << samples_received
              << " samples\n";

    // ------------------------------------------------------------
    // 9. Sample timing
    // ------------------------------------------------------------
    const double sample_period =
        1.0 / rx_sample_rate;

    std::cout << "\nSample period: "
              << sample_period
              << " seconds\n";

    std::cout << "Sample period: "
              << sample_period * 1e6
              << " microseconds\n";

    // ------------------------------------------------------------
    // 10. Print individual sample timestamps
    // ------------------------------------------------------------
    if (metadata.has_time_spec)
    {
        std::cout << "\nFirst 10 samples with timestamps:\n";

        for (std::size_t n = 0;
             n < std::min<std::size_t>(10, samples_received);
             ++n)
        {
            const double sample_time =
                first_sample_time +
                static_cast<double>(n) * sample_period;

            std::cout
                << "sample[" << n << "]"
                << "  timestamp = "
                << sample_time
                << " s"
                << "  value = ("
                << buffer[n].real()
                << ", "
                << buffer[n].imag()
                << ")\n";
        }
    }

    // ------------------------------------------------------------
    // 11. Stop RX
    // ------------------------------------------------------------
    stream_cmd.stream_mode =
        uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

    rx_stream->issue_stream_cmd(stream_cmd);

    std::cout << "\nRX streaming stopped.\n";

    return 0;
}


