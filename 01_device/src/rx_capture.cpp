#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

/*
 * Record received samples to disk for offline analysis.
 *
 * Writes raw interleaved complex float32 -- the same layout UHD hands
 * back for "fc32" -- so numpy reads it with np.fromfile(dtype=
 * complex64) and MATLAB with fread(...,'float32') reshaped into
 * complex pairs. A sidecar .txt carries the rate, frequency, gain and
 * the device timestamp of the first sample, because a capture without
 * its sample rate is not analysable.
 *
 * Note the disk rate this implies: at 30.72 MS/s, fc32 is 8 bytes per
 * sample, so 246 MB/s. A spinning disk will not keep up and even an
 * SSD may not sustain it. For high rates capture into /dev/shm, which
 * is RAM, and keep the run short:
 *
 *   ./rx_capture /dev/shm/cap.fc32 2 30.72e6 3515 50
 *
 * Overflows are counted and reported. A capture with overflows has
 * missing samples in the middle and its timing cannot be trusted.
 */

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr
            << "Usage:\n"
            << "  ./rx_capture <outfile> [seconds] [rate] [freq_MHz] [gain]\n\n"
            << "Defaults: 2 s, 30.72 MS/s, 3515 MHz, gain 50\n\n"
            << "For rates above a few MS/s write into /dev/shm:\n"
            << "  ./rx_capture /dev/shm/cap.fc32 2 30.72e6 3515 50\n";

        return 1;
    }

    const std::string path = argv[1];

    const double seconds = argc >= 3 ? std::stod(argv[2]) : 2.0;
    const double rate    = argc >= 4 ? std::stod(argv[3]) : 30.72e6;
    const double freq    = argc >= 5 ? std::stod(argv[4]) * 1e6 : 3515e6;
    const double gain    = argc >= 6 ? std::stod(argv[5]) : 50.0;

    auto usrp = uhd::usrp::multi_usrp::make(std::string(""));

    set_master_clock_for(usrp, rate);

    usrp->set_rx_rate(rate);
    usrp->set_rx_gain(gain);

    const std::string otw = otw_format();

    auto rx = usrp->get_rx_stream(uhd::stream_args_t("fc32", otw));

    usrp->set_rx_freq(uhd::tune_request_t(freq), 0);

    const double actual_rate = usrp->get_rx_rate();

    const unsigned long long want =
        static_cast<unsigned long long>(actual_rate * seconds);

    std::cout
        << std::fixed << std::setprecision(6)
        << "master clock : " << usrp->get_master_clock_rate() / 1e6 << " MHz\n"
        << "rate         : " << actual_rate / 1e6 << " MS/s"
        << "  (asked " << rate / 1e6 << ")\n"
        << std::setprecision(3)
        << "freq         : " << usrp->get_rx_freq() / 1e6 << " MHz\n"
        << "gain         : " << usrp->get_rx_gain() << " dB\n"
        << "antenna      : " << usrp->get_rx_antenna() << "\n"
        << "wire format  : " << otw << "\n"
        << "duration     : " << seconds << " s -> " << want << " samples, "
        << double(want) * 8.0 / 1e6 << " MB\n"
        << "file         : " << path << "\n\n";

    std::ofstream f(path, std::ios::binary);

    if (!f)
    {
        std::cerr << "cannot open " << path << " for writing\n";
        return 1;
    }

    std::vector<char> filebuf(1 << 22);              // 4 MB of buffering
    f.rdbuf()->pubsetbuf(filebuf.data(), filebuf.size());

    std::vector<complex_t> buf(rx->get_max_num_samps());

    uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    cmd.stream_now = true;
    rx->issue_stream_cmd(cmd);

    unsigned long long got_total = 0;
    size_t overflows = 0, errors = 0;

    double first_t = -1.0;

    const auto t0 = clock_type::now();

    while (got_total < want)
    {
        uhd::rx_metadata_t md;

        const size_t got = rx->recv(buf.data(), buf.size(), md, 1.0);

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            ++overflows;
            continue;
        }

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            ++errors;
            continue;
        }

        if (got == 0) continue;

        if (first_t < 0.0 && md.has_time_spec)
            first_t = md.time_spec.get_real_secs();

        f.write(
            reinterpret_cast<const char*>(buf.data()),
            std::streamsize(got * sizeof(complex_t)));

        got_total += got;
    }

    cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx->issue_stream_cmd(cmd);

    uhd::rx_metadata_t flush;
    while (rx->recv(buf.data(), buf.size(), flush, 0.1) > 0) {}

    f.close();

    const double wall =
        std::chrono::duration<double>(clock_type::now() - t0).count();

    /*
     * The sidecar. A capture without its sample rate cannot be
     * analysed, and a capture with overflows should not be trusted.
     */
    std::ofstream meta(path + ".txt");

    meta << std::fixed << std::setprecision(6)
         << "format=fc32_interleaved_float32\n"
         << "rate_hz=" << actual_rate << "\n"
         << "freq_hz=" << usrp->get_rx_freq() << "\n"
         << "gain_db=" << usrp->get_rx_gain() << "\n"
         << "antenna=" << usrp->get_rx_antenna() << "\n"
         << "otw=" << otw << "\n"
         << "samples=" << got_total << "\n"
         << "first_sample_device_time_s=" << first_t << "\n"
         << "overflows=" << overflows << "\n"
         << "errors=" << errors << "\n";
    meta.close();

    std::cout
        << "captured     : " << got_total << " samples in "
        << std::setprecision(3) << wall << " s ("
        << double(got_total) / wall / 1e6 << " MS/s to disk)\n"
        << "overflows    : " << overflows << "\n"
        << "other errors : " << errors << "\n"
        << "sidecar      : " << path << ".txt\n";

    if (overflows)
    {
        std::cout
            << "\nWARNING: samples were dropped mid-capture, so the file has\n"
            << "gaps and its timing is not continuous. Capture into /dev/shm,\n"
            << "shorten the run, or lower the rate.\n";
    }
    else
    {
        std::cout << "\nClean capture: no gaps.\n";
    }

    return 0;
}
