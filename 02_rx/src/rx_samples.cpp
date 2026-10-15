#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

/*
 * Receive samples, and measure how strong they are.
 *
 *   ./rx_samples [seconds] [rate] [freq_MHz] [gain]
 *
 * Defaults to 0.1 s at 30.72 MS/s, 3515 MHz, 30 dB.
 *
 * On measuring received power -- the short version is that the B210 has no
 * usable RSSI, so this computes power from the samples instead.
 *
 * UHD does expose an "rssi" sensor for this device, and it is a trap. It
 * reads the AD9361's raw RSSI register, which is referred to the internal
 * gain state rather than to the antenna, and it is not calibrated. Measured
 * here with a steady transmitter on the other radio:
 *
 *     RX gain 20 dB -> rssi -71.75, samples at -46.4 dBFS
 *     RX gain 40 dB -> rssi -81.50, samples at -34.4 dBFS
 *     RX gain 50 dB -> rssi -81.75, samples at -24.7 dBFS
 *
 * The input power never changed, so a real input-referred RSSI would have
 * held still; instead it fell 10 dB while the samples rose 22 dB. It is
 * printed below for interest, clearly marked, and should not be used.
 *
 * uhd::has_rx_power_reference() -- the calibrated absolute-power API -- also
 * returns false on this device, since there is no factory power calibration.
 *
 * What is trustworthy is the samples themselves. Power relative to converter
 * full scale is exact:
 *
 *     P_dBFS = 20*log10(rms of the complex samples)
 *
 * Converting that to dBm at the antenna needs a one-time calibration against
 * a known source; see the note the program prints.
 */

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    const double seconds = argc >= 2 ? std::stod(argv[1]) : 0.1;
    const double rate    = argc >= 3 ? std::stod(argv[2]) : 30.72e6;
    const double freq    = argc >= 4 ? std::stod(argv[3]) * 1e6 : 3515e6;
    const double gain    = argc >= 5 ? std::stod(argv[4]) : 30.0;

    const size_t channel = 0;

    auto usrp = uhd::usrp::multi_usrp::make(std::string(""));

    /*
     * The B210 divides its master clock to make the sample rate, and the
     * 32 MHz default is not a multiple of 30.72 MS/s -- asking for that rate
     * against it silently yields 32 MS/s instead. Move the master clock to
     * match, which gives a divisor of one.
     */
    set_master_clock_for(usrp, rate);

    usrp->set_rx_rate(rate, channel);
    usrp->set_rx_gain(gain, channel);

    uhd::stream_args_t stream_args("fc32", otw_format());
    stream_args.channels = std::vector<size_t>(1, channel);

    auto rx_stream = usrp->get_rx_stream(stream_args);

    usrp->set_rx_freq(uhd::tune_request_t(freq), channel);

    const double actual_rate = usrp->get_rx_rate(channel);

    std::cout
        << std::fixed << std::setprecision(6)
        << "master clock : " << usrp->get_master_clock_rate() / 1e6 << " MHz\n"
        << "rate         : " << actual_rate / 1e6 << " MS/s"
        << "  (asked " << rate / 1e6 << ")\n"
        << std::setprecision(3)
        << "freq         : " << usrp->get_rx_freq(channel) / 1e6 << " MHz\n"
        << "gain         : " << usrp->get_rx_gain(channel) << " dB"
        << "  (range 0 - " << usrp->get_rx_gain_range().stop() << ")\n"
        << "antenna      : " << usrp->get_rx_antenna(channel) << "\n"
        << "wire format  : " << otw_format() << "\n";

    const size_t want =
        static_cast<size_t>(actual_rate * seconds);

    std::cout
        << "collecting   : " << want << " samples ("
        << seconds * 1e3 << " ms)\n";

    /*
     * recv() returns at most one packet per call -- about 2040 samples here,
     * whatever size buffer it is handed -- so collecting a given number of
     * samples means looping. Asking for a million in a single call and
     * believing the answer is a common way to think you captured far more
     * than you did.
     */
    std::vector<complex_t> chunk(rx_stream->get_max_num_samps());

    std::cout
        << "max per recv : " << chunk.size() << " samples\n\n";

    std::vector<complex_t> samples;
    samples.reserve(want);

    uhd::stream_cmd_t cmd(
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    cmd.stream_now = false;
    cmd.time_spec  = usrp->get_time_now() + 0.1;

    rx_stream->issue_stream_cmd(cmd);

    size_t calls = 0, overflows = 0, errors = 0;

    double first_t = -1.0;

    bool first_md_shown = false;

    while (samples.size() < want)
    {
        uhd::rx_metadata_t md;

        const size_t got =
            rx_stream->recv(chunk.data(), chunk.size(), md, 1.0);

        ++calls;

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
        {
            ++overflows;
            continue;
        }

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            ++errors;

            std::cout
                << "  RX error: "
                << rx_error_to_string(md.error_code) << "\n";

            continue;
        }

        if (got == 0) continue;

        if (!first_md_shown)
        {
            std::cout
                << "First packet metadata:\n"
                << "  error_code     : "
                << rx_error_to_string(md.error_code) << "\n"
                << "  has_time_spec  : " << md.has_time_spec << "\n"
                << "  more_fragments : " << md.more_fragments << "\n"
                << "  samples        : " << got << "\n";

            if (md.has_time_spec)
            {
                first_t = md.time_spec.get_real_secs();

                std::cout
                    << std::setprecision(9)
                    << "  time_spec      : " << first_t << " s\n"
                    << std::setprecision(3);
            }

            first_md_shown = true;
        }

        samples.insert(
            samples.end(),
            chunk.begin(),
            chunk.begin() + std::min(got, want - samples.size()));
    }

    cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(cmd);

    uhd::rx_metadata_t flush;
    while (rx_stream->recv(chunk.data(), chunk.size(), flush, 0.1) > 0) {}

    // ----------------------------------------------------------------
    // Power, computed from the samples
    // ----------------------------------------------------------------
    double sum_sq = 0.0, peak = 0.0;

    for (const auto& c : samples)
    {
        const double p = std::norm(c);           // |c|^2

        sum_sq += p;
        peak = std::max(peak, p);
    }

    const double rms =
        std::sqrt(sum_sq / double(samples.size()));

    const double peak_mag = std::sqrt(peak);

    const double dbfs = 20.0 * std::log10(std::max(rms, 1e-12));
    const double peak_dbfs = 20.0 * std::log10(std::max(peak_mag, 1e-12));

    std::cout
        << "\nReceived " << samples.size() << " samples in "
        << calls << " recv() calls"
        << "  (overflows " << overflows
        << ", errors " << errors << ")\n";

    std::cout
        << std::setprecision(1)
        << "\nSignal level, from the samples:\n"
        << std::setprecision(6)
        << "  rms            = " << rms << "\n"
        << std::setprecision(2)
        << "  rms power      = " << dbfs << " dBFS\n"
        << "  peak           = " << peak_mag
        << "  (" << peak_dbfs << " dBFS)\n"
        << "  crest factor   = " << peak_dbfs - dbfs << " dB\n"
        << "  headroom       = " << -peak_dbfs << " dB below clipping\n";

    if (peak_mag > 0.95)
    {
        std::cout
            << "\n  WARNING: peaks are at full scale -- the converter is\n"
            << "  clipping and these samples are distorted. Lower the gain.\n";
    }

    // ----------------------------------------------------------------
    // The RSSI sensor, for interest only
    // ----------------------------------------------------------------
    const auto sensors = usrp->get_rx_sensor_names(channel);

    if (std::find(sensors.begin(), sensors.end(), "rssi") != sensors.end())
    {
        const auto s = usrp->get_rx_sensor("rssi", channel);

        std::cout
            << "\nDevice \"rssi\" sensor : " << s.value << " " << s.unit
            << "   <-- do not use\n"
            << "  It is the AD9361's raw register, referred to the internal\n"
            << "  gain state rather than to the antenna, and uncalibrated.\n"
            << "  Measured against a steady transmitter, it fell 10 dB while\n"
            << "  the samples rose 22 dB as the gain was raised. The dBFS\n"
            << "  figure above is the one to trust.\n";
    }

    std::cout
        << "\nTurning dBFS into dBm needs a one-time calibration, because\n"
        << "this device has no factory power reference"
        << " (has_rx_power_reference = "
        << (usrp->has_rx_power_reference(channel) ? "true" : "false") << "):\n"
        << "  1. feed a known level, say -40 dBm, from a signal generator\n"
        << "  2. note the dBFS this program reports at a fixed gain\n"
        << "  3. offset = known_dBm - measured_dBFS\n"
        << "then dBm = dBFS + offset, valid for that gain and frequency.\n";

    return 0;
}
