# Known issues

## Intermittent: the radio sometimes stops emitting until the device is reset

**Status: understood well enough to work around, root cause unknown.**

The C++ transmitter *does* work. `zctx` on one NUC and `zcrx` on the other
detect reliably, and three consecutive runs measured rms 0.02835 / 0.02837 /
0.02850 at the far end. That figure matches `tools/zc_tx.py` (0.02838) to
within a fraction of a percent, which is what a correct ZC burst at 25% duty
and 0.7 amplitude should produce.

But the B210 can get into a state where **nothing transmits at all**, and it
survives restarting the program. In that state:

- `send()` still returns full sample counts and still paces at exactly the
  configured rate,
- `get_tx_freq()`, `get_tx_gain()` and `get_tx_rate()` read back what was asked,
- the `lo_locked` sensor reads true,
- and a receiver on the far end of the cable stays at the noise floor.

Nothing reports an error. The only way to notice is to measure the far end.

It is not a code defect, and it is not UHD. During one long bad spell, a
minimal C++ reproducer, a rebuilt UHD example and our own app all stayed silent
while Python transmitted; later, with no code change at all, the identical
binaries reached 82-83 dB SNR. Several plausible-looking "fixes" were found and
then disproved this way -- streamer/tune ordering, `tune_request_t(freq)` vs
`tune_request_t(freq, lo_offset)`, explicit `stream_args.channels`, the
buffer-pointer form passed to `send()`, a first-packet time spec, a settling
delay, RX configuration, and send buffer size. **Each of these appeared to fix
the problem and none of them did.** Do not rebuild UHD over this; UHD's own
`tx_waveforms.cpp` compiled locally transmits fine here.

### The unclean-shutdown guess was wrong

An earlier version of this file blamed killing transmitting processes
mid-stream. That was tested on 2026-09-18 and **disproved**: after a SIGKILL
mid-burst, and again after five rapid start/kill cycles, a fresh `zctx` still
transmitted normally (rms 0.02847 and 0.02839). The trigger is still unknown,
and the failure could not be reproduced on demand at all that day -- including
from a cold start after the boxes had been idle overnight.

One thing never recorded during a bad spell was device temperature. The long
failure ran after hours of continuous TX at 80 dB gain, so thermal protection
is a live but untested hypothesis. Idle baseline for comparison is
`tx temp = 44.9 C` (read via `get_tx_sensor("temp", 0)`).

### If it comes back, capture this before changing anything

The failure is the only chance to identify it, so do not start editing:

1. Read `tx temp` and compare with the ~45 C idle baseline.
2. With our app still silent, immediately run
   `/usr/libexec/uhd/examples/tx_waveforms --freq 3515e6 --rate 1e6 --gain 80
   --wave-type SINE --wave-freq 100e3 --ampl 0.7` and measure. If that
   transmits while ours does not, the split is real and reproducible at that
   moment -- which is the one condition under which the cause can be bisected.
3. Only then power-cycle the B210 (unplug the USB) to recover.

### How to tell, in one command

With a transmitter running on the other box:

    python3 /tmp/ports.py check        # rms ~0.028 for ZC, ~0.060 for a CW tone

Noise floor is rms ~0.00061. If you see that while something is transmitting,
the radio is in the bad state -- reset it rather than debugging the code.

## Measurement discipline

Most of the wrong conclusions above came from sloppy measurement, so:

1. Kill every transmitter and confirm zero survivors.
2. Measure a baseline and check it is at the noise floor (~0.00061).
3. Only then start the program under test, and confirm it is alive.

A strong CW tone at a frequency your program does not generate means a stale
transmitter is still running. Note `pkill -f foo` matches the ssh shell running
it and kills your session -- use `pkill -f "[f]oo"`.

Also beware the **stream-startup transient**: the first window after any stream
start correlates against anything and fakes a detection at offset 0-12 with
peak-to-mean 7-23. Discard roughly the first 100k samples before deciding
whether a signal is really present.

## Use TX gain 80

At 60 dB and below the signal is too weak to detect over the cable and
attenuator. This is the first thing to check if nothing is being received.
