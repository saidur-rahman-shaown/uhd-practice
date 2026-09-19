# UHD B210 latency experiments

Two Ubuntu NUCs, each with a USRP B210, cabled TX/RX -> RX2 in both directions
with attenuation in line. Build and run on the NUCs; the Mac has no UHD and is
for editing and git only.

| host | ssh | serial | name |
|---|---|---|---|
| grid3 | `shaown@192.168.0.102` | 310733D | grid3 |
| MyB210 | `shaown@192.168.0.103` | 310731F | MyB210 |

## Build

```bash
ssh shaown@192.168.0.102                 # or .103
cd ~/uhd-practice/01_device
git pull
cmake --build build                      # first time: cmake -B build -G Ninja .
                                         # builds latency_test and tdd_latency_test
```

## Single box, no RF needed

Safe to run anywhere, nothing depends on the cable.

```bash
./build/latency_test zcseg     # detector self-test, pure maths, no radio
./build/latency_test recv      # host recv() timing
./build/latency_test rxmeta    # deliberately provokes OVERFLOW and LATE_COMMAND
./build/latency_test send      # host send() timing
./build/latency_test txmeta    # TX async events: BURST_ACK, UNDERFLOW, TIME_ERROR
```

`txmeta` schedules every fourth burst in the past on purpose, so those
iterations *should* come back as TIME_ERROR. If they do not, the device is not
reporting async messages and the rest of the numbers mean nothing.

## Two boxes, over the cable

Sender on one, receiver on the other. **Use TX gain 80** -- 60 and below is too
weak to detect through the attenuator.

```bash
# on .102
cd ~/uhd-practice/01_device && ./build/latency_test zctx 40 80

# on .103, while that is running
cd ~/uhd-practice/01_device && ./build/latency_test zcrx 15 4 4.0 50
```

Arguments: `zctx [seconds] [tx_gain]`,
`zcrx [seconds] [segments] [threshold] [rx_gain]`.

Healthy output is peak-to-mean roughly 50-110, correlation offsets scattered
across the window, and a steady carrier offset (a few hundred Hz between the
two free-running TCXOs). Peak-to-mean stuck near 2.0 means nothing is being
received.

To run it from the Mac in one go:

```bash
ssh shaown@192.168.0.102 'cd ~/uhd-practice/01_device; \
  setsid nohup ./build/latency_test zctx 40 80 >/tmp/zctx.log 2>&1 </dev/null &'
sleep 8
ssh shaown@192.168.0.103 'cd ~/uhd-practice/01_device && \
  ./build/latency_test zcrx 15 4 4.0 50'
```

`setsid nohup ... </dev/null &` matters: without it the sender dies when ssh
returns.

## TDD slot timing

A separate program, `tdd_latency_test`, built from the same directory.

```bash
./build/tdd_latency_test stream 1.0 10 0.5   # slot_ms, seconds, duty  -- works
./build/tdd_latency_test slots  1.0 1000 20  # slot_ms, slots, pipeline_ms -- fails
./build/tdd_latency_test both
```

`slots` schedules one self-contained burst per slot. This is the obvious way to
build a frame and it does not work: at a 1 ms cadence only half the slots are
acknowledged, and neither a deeper pipeline nor a longer slot helps. The device
cannot tear down and re-arm the transmit chain every slot. It is kept because
seeing it fail is the point.

`stream` is the pattern to use. The burst is opened once, only the first sample
carries a timestamp, and the slot structure lives in the samples -- signal
during the TX portion, zeros during the rest. Measured over 10016 slots at
1 ms: zero underflows, zero late packets. Slot edges are sample counts inside
one stream, so they cannot drift, and the host has no per-slot deadline to miss.

Related, in `latency_test`:

```bash
./build/latency_test lead 1.0 50     # minimum safe lead, async-verified
```

This sweeps scheduling lead times and judges each burst by the device's own
async report rather than by `send()` returning -- `send()` accepts a burst whose
time has already passed and the device drops it silently. Measured: 0.2 ms lead
is late in 49 of 50 bursts, 0.5 ms and above is clean.

## Is anything actually being transmitted?

The check that settles device-versus-code arguments. On the receiving box:

```bash
python3 /tmp/ports.py check
```

| reading | meaning |
|---|---|
| rms ~0.00061 | noise floor, nothing arriving |
| rms ~0.028 | ZC burst arriving normally |
| rms ~0.060 | CW tone arriving normally |

If a transmitter is running and you still see the noise floor, the radio is in
the silent state described in KNOWN_ISSUES.md -- power-cycle the B210 rather
than editing code. `tools/zc_tx.py 40 80` is a known-good reference sender for
the same purpose.

## Modes that do not work

- `zc` -- its live half transmits and receives on one box, which needs a TX->RX
  loopback that is not cabled here. Its offline self-test still passes and is
  worth running.
- `txrx` -- reports negative latency. It compares the RX window's start time
  against the scheduled TX time instead of correlating for the burst, so the
  figure is meaningless. The `zctx`/`zcrx` path is the correct replacement.

## Housekeeping

Before trusting any measurement, kill every transmitter and confirm the
receiver reads the noise floor first. A strong tone at a frequency you did not
generate means a stale sender is still running.

```bash
ssh shaown@192.168.0.102 'pkill -x latency_test; pkill -f "[z]c_tx.py"'
```

Note the bracket: `pkill -f zc_tx.py` matches the ssh shell running it and
kills your own session.
