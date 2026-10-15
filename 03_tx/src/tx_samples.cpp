#include "common.hpp"

#include <uhd/utils/safe_main.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

/*
 * Transmit a Zadoff-Chu burst once every period, for a given run length.
 *
 *   ./tx_samples [seconds] [period_ms] [gain] [rate] [zc_length] [zc_root]
 *
 * Defaults to 5 s of bursts every 10 ms at 30.72 MS/s -- 500 bursts of 401
 * samples, which at that rate is 13.05 us of signal in each 10 ms frame, a
 * duty cycle of about 0.13%.
 *
 * Each burst is self-contained: start- and end-of-burst set, with an absolute
 * timestamp. That is the right structure at this cadence, and it is worth
 * knowing why it would be the wrong one if the period were much shorter.
 * Below roughly 10 ms the device cannot tear down and re-arm the transmit
 * chain fast enough, and about half the bursts are dropped -- measured at
 * exactly 50% for 1 ms and 2 ms periods, unchanged by deeper queueing. For
 * those cadences the frame has to be one continuous stream with the structure
 * written into the samples; see 06_tdd. At 10 ms, per-burst scheduling is
 * clean, and it gives something continuous streaming does not: the device
 * reports on every individual burst, so the run can be verified rather than
 * assumed.
 *
 * Timestamps come from a single anchor, so the cadence cannot drift with host
 * scheduling. The host paces itself roughly with a sleep, which only keeps the
 * queue shallow -- the timing comes from the device clock.
 */

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    const double seconds   = argc >= 2 ? std::stod(argv[1]) : 5.0;
    const double period_ms = argc >= 3 ? std::stod(argv[2]) : 10.0;
    const double gain      = argc >= 4 ? std::stod(argv[3]) : 80.0;
    const double rate      = argc >= 5 ? std::stod(argv[4]) : 30.72e6;
    const size_t zc_length = argc >= 6 ? std::stoul(argv[5]) : 401;
    const size_t zc_root   = argc >= 7 ? std::stoul(argv[6]) : 25;

    const size_t channel = 0;

    if (std::gcd(zc_root, zc_length) != 1)
    {
        std::cerr
            << "WARNING: root " << zc_root << " is not coprime to length "
            << zc_length << "; the correlation will not be ideal.\n";
    }

    auto usrp = uhd::usrp::multi_usrp::make(std::string(""));

    /*
     * The B210 divides its master clock to make the sample rate, and the
     * 32 MHz default is not a multiple of 30.72 MS/s -- asking for that rate
     * against it silently gives 32 instead.
     */
    set_master_clock_for(usrp, rate);

    usrp->set_tx_rate(rate, channel);
    usrp->set_tx_gain(gain, channel);

    uhd::stream_args_t stream_args("fc32", otw_format());
    stream_args.channels = std::vector<size_t>(1, channel);

    auto tx_stream = usrp->get_tx_stream(stream_args);

    usrp->set_tx_freq(uhd::tune_request_t(3515e6), channel);

    const double actual_rate = usrp->get_tx_rate(channel);
    const double period      = period_ms / 1e3;

    const size_t bursts =
        static_cast<size_t>(seconds / period);

    /*
     * Zadoff-Chu, scaled off full scale: it is constant modulus at 1.0, which
     * would leave the converter no headroom.
     */
    const float ampl = 0.7f;

    std::vector<complex_t> burst(zc_length);

    for (size_t n = 0; n < zc_length; ++n)
    {
        const unsigned long long num =
            static_cast<unsigned long long>(zc_root)
            * static_cast<unsigned long long>(n)
            * static_cast<unsigned long long>(n + 1);

        const double phase =
            -M_PI
            * static_cast<double>(num % (2ULL * zc_length))
            / static_cast<double>(zc_length);

        burst[n] = complex_t(
            ampl * static_cast<float>(std::cos(phase)),
            ampl * static_cast<float>(std::sin(phase)));
    }

    std::cout
        << std::fixed << std::setprecision(6)
        << "master clock : " << usrp->get_master_clock_rate() / 1e6 << " MHz\n"
        << "rate         : " << actual_rate / 1e6 << " MS/s"
        << "  (asked " << rate / 1e6 << ")\n"
        << std::setprecision(3)
        << "freq         : " << usrp->get_tx_freq(channel) / 1e6 << " MHz\n"
        << "gain         : " << usrp->get_tx_gain(channel) << " dB\n"
        << "antenna      : " << usrp->get_tx_antenna(channel) << "\n"
        << "wire format  : " << otw_format() << "\n"
        << "ZC           : length " << zc_length << ", root " << zc_root
        << ", amplitude " << ampl << "\n"
        << "burst        : " << zc_length << " samples = "
        << double(zc_length) / actual_rate * 1e6 << " us\n"
        << "period       : " << period_ms << " ms  (duty "
        << 100.0 * double(zc_length) / actual_rate / period << " %)\n"
        << "run          : " << seconds << " s = " << bursts << " bursts\n\n";

    /*
     * One anchor, far enough ahead that the first burst is not already late.
     */
    const uhd::time_spec_t t0 = usrp->get_time_now() + 0.2;

    size_t acks = 0, late = 0, underflows = 0, other = 0, short_sends = 0;

    auto drain = [&](double timeout) {
        uhd::async_metadata_t am;
        while (tx_stream->recv_async_msg(am, timeout))
        {
            switch (am.event_code)
            {
                case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                    ++acks; break;
                case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                    ++late; break;
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                    ++underflows; break;
                default:
                    ++other; break;
            }
        }
    };

    const auto wall0 = clock_type::now();

    for (size_t k = 0; k < bursts; ++k)
    {
        uhd::tx_metadata_t md;

        md.start_of_burst = true;
        md.end_of_burst   = true;
        md.has_time_spec  = true;
        md.time_spec      = t0 + uhd::time_spec_t(double(k) * period);

        const size_t sent =
            tx_stream->send(burst.data(), burst.size(), md, 1.0);

        if (sent != burst.size()) ++short_sends;

        drain(0.0);

        /*
         * Pace to roughly the period so the device queue stays shallow. The
         * timestamps above are what actually place the bursts, so a sleep
         * that runs a little long or short does not move them.
         */
        const double elapsed =
            std::chrono::duration<double>(
                clock_type::now() - wall0).count();

        const double target = double(k + 1) * period;

        if (target > elapsed)
        {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(target - elapsed));
        }
    }

    /*
     * The last bursts are still in flight; wait out the anchor lead, then
     * collect the remaining reports.
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    drain(0.1);

    const double wall =
        std::chrono::duration<double>(clock_type::now() - wall0).count();

    std::cout
        << "Sent " << bursts << " bursts in "
        << std::setprecision(3) << wall << " s"
        << "  (expected " << seconds << ")\n\n"
        << "Device reports:\n"
        << "  burst ACK    = " << acks << " of " << bursts << "\n"
        << "  late         = " << late << "\n"
        << "  underflow    = " << underflows << "\n"
        << "  other        = " << other << "\n"
        << "  short sends  = " << short_sends << "\n";

    const double pct =
        bursts ? 100.0 * double(acks) / double(bursts) : 0.0;

    std::cout
        << std::setprecision(1)
        << "  on time      = " << pct << " %\n";

    std::cout << "\nInterpretation:\n";

    if (acks == bursts && late == 0 && underflows == 0)
    {
        std::cout
            << "Every burst was acknowledged, so all " << bursts
            << " went out at their\nscheduled times. Note that send()"
            << " returning is not what shows this --\nit accepts a burst"
            << " whose time has already passed and the device then\ndrops it"
            << " silently. The acknowledgements above are the evidence.\n";
    }
    else if (late > 0)
    {
        std::cout
            << late << " bursts arrived after their scheduled time and were\n"
            << "dropped. Either the host fell behind, or the period is short\n"
            << "enough that the transmit chain cannot be re-armed in time --\n"
            << "below about 10 ms expect to lose half of them, and build the\n"
            << "frame as one continuous stream instead (see 06_tdd).\n";
    }
    else
    {
        std::cout
            << "Some bursts went unreported. If the count is small this is\n"
            << "the tail still in flight; if it is large, check for\n"
            << "underflows above.\n";
    }

    return 0;
}
