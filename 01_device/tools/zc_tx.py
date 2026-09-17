#!/usr/bin/env python3
"""Zadoff-Chu transmitter for the two-host experiment.

Pairs with `latency_test zcrx` on the other NUC:

    # on the transmitting box
    ./tools/zc_tx.py 40 80
    # on the receiving box
    ./build/latency_test zcrx 15 4 4.0 50

This exists because the C++ transmitter in latency_test.cpp does not emit RF
on these B210s, while this does -- see KNOWN_ISSUES.md. The waveform matches
make_zadoff_chu(401, 25) in latency_test.cpp exactly, so the C++ detector
correlates against it directly.
"""
import sys
import time

import numpy as np
import uhd

N_ZC = 401          # sequence length, must match the C++ side
ROOT = 25           # ZC root, must match the C++ side
FREQ = 3515e6
RATE = 1e6
AMPL = 0.7          # keep off full scale; ZC is constant modulus at 1.0


def zadoff_chu(length=N_ZC, root=ROOT):
    n = np.arange(length)
    return np.exp(-1j * np.pi * root * n * (n + 1) / length)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    gain = float(sys.argv[2]) if len(sys.argv) > 2 else 80.0

    zc = zadoff_chu()

    # One burst is the sequence followed by silence, so the receiver sees a
    # clear gap between repetitions. Batch several into one send: feeding the
    # device a single 1604-sample burst per call starves the TX chain.
    burst = np.concatenate([zc, np.zeros(N_ZC * 3)])
    buf = (np.tile(burst, 10) * AMPL).astype(np.complex64)

    usrp = uhd.usrp.MultiUSRP("")
    usrp.set_tx_rate(RATE)
    usrp.set_tx_freq(uhd.types.TuneRequest(FREQ))
    usrp.set_tx_gain(gain)

    print(f"TX  freq={usrp.get_tx_freq()/1e6:.3f} MHz  "
          f"rate={usrp.get_tx_rate()/1e6:.3f} MS/s  "
          f"gain={usrp.get_tx_gain():.1f} dB  "
          f"ant={usrp.get_tx_antenna()}")
    print(f"ZC length={N_ZC} root={ROOT}, {len(buf)} samples per send")

    stream = usrp.get_tx_stream(uhd.usrp.StreamArgs("fc32", "sc16"))

    md = uhd.types.TXMetadata()
    md.start_of_burst = True
    md.end_of_burst = False

    sent = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        sent += stream.send(buf, md)
        md.start_of_burst = False

    md.end_of_burst = True
    stream.send(np.zeros(0, dtype=np.complex64), md)

    print(f"sent {sent} samples in {seconds:.0f} s")


if __name__ == "__main__":
    main()
