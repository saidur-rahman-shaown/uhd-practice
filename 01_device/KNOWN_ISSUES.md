# Known issues

## The C++ transmitter in `latency_test.cpp` does not emit RF

**Status: open.** Affects every transmitting mode (`send`, `lead`, `txrx`,
`txmeta`, `zc`, `zctx`).

`tx_stream->send()` returns full sample counts and paces correctly at the
configured rate, `get_tx_freq()` / `get_tx_gain()` / `get_tx_rate()` all read
back exactly what was requested, and the `lo_locked` sensor reads true -- but a
receiver on the far end of the cable stays at the noise floor.

### What has been ruled out

Measured over the inter-host cable at 3515 MHz, 80 dB TX gain, with every other
transmitter killed and verified gone before each reading:

| transmitter | result |
|---|---|
| nothing (baseline) | rms 0.00061 |
| Python (`uhd` bindings) | **81.7 dB SNR** |
| `/usr/libexec/uhd/examples/tx_waveforms` (distro C++ binary) | **81.9 dB SNR** |
| UHD's own `tx_waveforms.cpp`, compiled locally with g++ 15.2 | **82.7 dB SNR** |
| our minimal ~60-line C++ reproducer | rms 0.00061 (noise) |

So this is **not** a UHD install, library, ABI or toolchain problem: UHD's own
source compiles here and transmits. Do not rebuild UHD from source -- it was
tried as a hypothesis and disproved before spending the time.

Also individually ruled out, each tested in isolation against a verified
baseline:

- creating the TX streamer before vs. after tuning
- setting `stream_args.channels` explicitly
- passing `send()` a `std::vector<T*>` rather than a bare pointer
- a time spec on the first packet, plus `set_time_now(0)`
- a one second settling delay before streaming
- configuring RX alongside TX
- send buffer size (4k, 10k, 16k samples)
- `set_tx_rate` with and without a channel index

### Measurement discipline

Several wrong conclusions in this file's history came from a previous
transmitter still running during a measurement. Before trusting any reading:

1. kill every transmitter and confirm zero survivors,
2. measure a baseline and check it is at the noise floor,
3. only then start the program under test and confirm it is alive.

A CW tone reading a strong SNR at a frequency you did not intend is the
signature of a stale transmitter. Note also that `pkill -f foo` matches the ssh
shell running it and kills the session -- use `pkill -f "[f]oo"`.

### Workaround

Use `tools/zc_tx.py` as the transmitter. The receive side (`zcrx`) is correct
and verified: with the Python transmitter it detects reliably, reporting
peak-to-mean 76-108 with correlation offsets scattered across the window and a
stable ~280 Hz carrier offset between the two TCXOs.

    # transmitting box
    ./tools/zc_tx.py 40 80
    # receiving box
    ./build/latency_test zcrx 15 4 4.0 50

### Next step

Bisect from UHD's `tx_waveforms.cpp`, which is known to work here, by deleting
pieces until it stops transmitting, rather than by adding pieces to the minimal
reproducer.
