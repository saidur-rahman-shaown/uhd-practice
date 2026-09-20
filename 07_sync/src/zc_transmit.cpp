#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

/*
 * Transmit a Zadoff-Chu sequence continuously, and write the exact
 * reference waveform to disk so a capture can be correlated against
 * the same samples that were sent.
 *
 * Pair it with rx_capture on the other radio:
 *
 *   # sender
 *   ./zc_transmit 30 80 30.72e6 401 25
 *   # receiver
 *   ./rx_capture /dev/shm/cap.fc32 2 30.72e6 3515 50
 *
 * Then correlate offline. The reference is written as fc32, the same
 * format as the capture, so both load the same way.
 *
 * The sequence repeats back to back with no silent gap. Padding each
 * repetition with zeros stops these B210s emitting altogether, and
 * the gap buys nothing: the correlator finds one peak per repetition
 * either way, so the recovered offset is simply modulo the sequence
 * length.
 */

std::vector<complex_t> make_zc(size_t length, size_t root)
{
    std::vector<complex_t> seq(length);

    for (size_t n = 0; n < length; ++n)
    {
        /*
         * exp(-j*pi*k/N) repeats every k = 2N, so reduce first --
         * without it the phase argument loses precision for long
         * sequences.
         */
        const unsigned long long num =
            static_cast<unsigned long long>(root)
            * static_cast<unsigned long long>(n)
            * static_cast<unsigned long long>(n + 1);

        const double phase =
            -M_PI
            * static_cast<double>(num % (2ULL * length))
            / static_cast<double>(length);

        seq[n] = complex_t(
            static_cast<float>(std::cos(phase)),
            static_cast<float>(std::sin(phase)));
    }

    return seq;
}


int UHD_SAFE_MAIN(int argc, char* argv[])
{
    const double seconds   = argc >= 2 ? std::stod(argv[1]) : 30.0;
    const double gain      = argc >= 3 ? std::stod(argv[2]) : 80.0;
    const double rate      = argc >= 4 ? std::stod(argv[3]) : 30.72e6;
    const size_t zc_length = argc >= 5 ? std::stoul(argv[4]) : 401;
    const size_t zc_root   = argc >= 6 ? std::stoul(argv[5]) : 25;
    const double freq      = argc >= 7 ? std::stod(argv[6]) * 1e6 : 3515e6;

    const std::string ref_path =
        argc >= 8 ? argv[7] : "/tmp/zc_reference.fc32";

    if (std::gcd(zc_root, zc_length) != 1)
    {
        std::cerr
            << "WARNING: root " << zc_root << " is not coprime to length "
            << zc_length << "; the correlation will not be ideal.\n";
    }

    auto usrp = uhd::usrp::multi_usrp::make(std::string(""));

    set_master_clock_for(usrp, rate);

    usrp->set_tx_rate(rate);
    usrp->set_tx_gain(gain);

    const std::string otw = otw_format();

    auto tx = usrp->get_tx_stream(uhd::stream_args_t("fc32", otw));

    usrp->set_tx_freq(uhd::tune_request_t(freq), 0);

    const double actual_rate = usrp->get_tx_rate();

    const auto zc = make_zc(zc_length, zc_root);

    /*
     * Scale off full scale: Zadoff-Chu is constant modulus at 1.0,
     * which leaves the converter no headroom.
     */
    const float ampl = 0.7f;

    std::vector<complex_t> buffer;

    const size_t reps =
        std::max<size_t>(1, send_buffer_samples(actual_rate) / zc_length);

    buffer.reserve(reps * zc_length);

    for (size_t r = 0; r < reps; ++r)
        for (const auto& c : zc)
            buffer.push_back(c * ampl);

    std::cout
        << std::fixed << std::setprecision(6)
        << "master clock : " << usrp->get_master_clock_rate() / 1e6 << " MHz\n"
        << "rate         : " << actual_rate / 1e6 << " MS/s"
        << "  (asked " << rate / 1e6 << ")\n"
        << std::setprecision(3)
        << "freq         : " << usrp->get_tx_freq() / 1e6 << " MHz\n"
        << "gain         : " << usrp->get_tx_gain() << " dB\n"
        << "antenna      : " << usrp->get_tx_antenna() << "\n"
        << "wire format  : " << otw << "\n"
        << "ZC           : length " << zc_length << ", root " << zc_root
        << ", amplitude " << ampl << "\n"
        << "buffer       : " << buffer.size() << " samples ("
        << std::setprecision(1)
        << double(buffer.size()) / actual_rate * 1e3 << " ms, "
        << reps << " repetitions) per send\n";

    /*
     * Write the reference, unscaled, so offline correlation uses the
     * identical sequence rather than a reconstruction that might
     * disagree about the sign convention.
     */
    std::ofstream ref(ref_path, std::ios::binary);

    ref.write(
        reinterpret_cast<const char*>(zc.data()),
        std::streamsize(zc.size() * sizeof(complex_t)));

    ref.close();

    std::ofstream meta(ref_path + ".txt");

    meta << std::fixed << std::setprecision(6)
         << "format=fc32_interleaved_float32\n"
         << "zc_length=" << zc_length << "\n"
         << "zc_root=" << zc_root << "\n"
         << "amplitude_on_air=" << ampl << "\n"
         << "rate_hz=" << actual_rate << "\n"
         << "freq_hz=" << usrp->get_tx_freq() << "\n"
         << "gain_db=" << usrp->get_tx_gain() << "\n"
         << "repetition_period_samples=" << zc_length << "\n";
    meta.close();

    std::cout
        << "reference    : " << ref_path << " (+ .txt)\n\n"
        << "Transmitting for " << std::setprecision(1) << seconds << " s...\n";

    uhd::tx_metadata_t md;

    md.start_of_burst = true;
    md.end_of_burst   = false;
    md.has_time_spec  = false;

    size_t underflows = 0;
    unsigned long long sent = 0;

    const auto t0 = clock_type::now();

    while (std::chrono::duration<double>(
               clock_type::now() - t0).count() < seconds)
    {
        sent += tx->send(buffer.data(), buffer.size(), md, 1.0);

        md.start_of_burst = false;

        uhd::async_metadata_t am;

        while (tx->recv_async_msg(am, 0.0))
        {
            if (am.event_code
                    == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW
                || am.event_code
                    == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET)
                ++underflows;
        }
    }

    md.end_of_burst = true;
    tx->send(buffer.data(), 0, md);

    std::cout
        << "\nsent         : " << sent << " samples ("
        << sent / zc_length << " repetitions)\n"
        << "underflows   : " << underflows << "\n";

    if (underflows)
    {
        std::cout
            << "\nUnderflows mean gaps in what was actually transmitted.\n"
            << "Lower the rate, or check NOTES.md section 3.5.\n";
    }

    return 0;
}
