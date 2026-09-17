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

The bad spells correlated with repeatedly killing transmitting processes
mid-stream, so a plausible but unproven guess is that an unclean shutdown
latches the TX front end off. If it happens, power-cycle the B210 (unplug the
USB) before changing any code.

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
