# UHD B210 latency experiments

> **New here?** Read [NOTES.md](NOTES.md) first. It explains what each program
> does, records every trap we hit — several of which fail silently, with the
> API reporting success while the radio does nothing — and shows how to test
> the whole thing. This file is the quick command reference.


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
                                         # builds latency_test, tdd_latency_test,
                                         # and async_events
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

## Learning the status characters

While streaming, UHD writes a single character to stderr for each problem it
notices, inline and with no newline:

| char | meaning |
|---|---|
| `U` | underflow -- the transmitter ran out of samples mid-burst |
| `L` | late packet -- a burst arrived after its scheduled time |
| `S` | sequence error -- the device rejected a packet as out of order |
| `O` | overflow -- the receiver outran the host |
| `D` | dropped packet |

Seeing none of them is the healthy case, which makes them hard to learn from:
you normally meet them by accident. `async_events` provokes each one on demand
and prints the matching async message underneath, so the character and its
meaning appear together.

```bash
./build/async_events ack         # clean burst -- nothing printed, 10/10 ACK
./build/async_events late        # L, and TIME_ERROR
./build/async_events underflow   # U, and UNDERFLOW
./build/async_events overflow    # O, and ERROR_CODE_OVERFLOW
./build/async_events latecmd     # LATE_COMMAND on the receive side
./build/async_events all
```

The `late` demo is the one worth reading twice: `send()` reports success for
all 10 bursts while the device reports `TIME_ERROR` for all 10. `send()`
returning only means the host handed the samples over.

The overflow demo raises the sample rate to 20 MS/s for its duration -- at
1 MS/s the buffers absorb the stall and nothing is lost.

## TDD slot timing

A separate program, `tdd_latency_test`, built from the same directory.

```bash
./build/tdd_latency_test stream 0.5 10 0.5   # slot_ms, seconds, duty  -- works
./build/tdd_latency_test slots  0.5 1000 20  # slot_ms, slots, pipeline_ms -- fails
./build/tdd_latency_test both
```

Slot length defaults to 0.5 ms throughout -- one NR slot at 30 kHz subcarrier
spacing, the numerology these experiments target.

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

### Alternating TX/RX slots

```bash
./build/tdd_latency_test alt 0.5 10 0.1 80 20e6   # slot_ms, seconds, guard, tx_gain, rate
```

A real TDD frame -- the same radio transmits in one slot and listens in the
next. Both directions run as continuous streams anchored to one `set_time_now`,
never one command per slot. The transmitter sends signal during its own slots
and zeros during the listening slots; the receiver runs free and assigns each
sample to a slot by its timestamp.

Verified on air at the far NUC, which sees exactly the designed frame:

| slot | duty measured | on-time measured | on/off |
|---|---|---|---|
| 1.0 ms | 45.0 % | 0.900 ms | 43.8 dB |
| 0.5 ms | 45.0 % | 0.451 ms | 42.9 dB |

45% is correct for a 10% guard: 0.9 ms of transmission in a 2 ms TX+RX period.

Sustained rate with both directions running at once, 0.5 ms slots:

| rate each way | result |
|---|---|
| 5, 15, 20, 25 MS/s | clean |
| 30 MS/s | underflows |

### NR feasibility: 30 kHz SCS, 20 MHz-class, 30.72 MS/s

**Written but not yet run** -- the lab network was unreachable when this was
added, so it has never been compiled. Build it before trusting it.

```bash
./build/tdd_latency_test nr all 30.72e6 60 0.5 0.1 80
#                        ^stage ^rate   ^s ^slot ^guard ^gain
```

Runs four stages in order, because a failure at 30.72 MS/s means nothing until
you know which part gave way:

| stage | what it isolates |
|---|---|
| `txonly` | transmit ceiling |
| `rxonly` | receive ceiling |
| `both`   | USB and host with both directions live |
| `tdd`    | the above plus the 0.5 ms slot structure |

Any stage can be run alone, e.g. `nr both 30.72e6 60`.

At 30.72 MS/s and a 0.5 ms slot the geometry is 15360 samples per slot, 1536
guard, 13824 transmitting -- which is 450 us of signal and 50 us of guard.

Every figure is measured rather than configured. The requested rate is what UHD
was asked for, the achieved rate is what UHD granted, the device rate is
derived from receive timestamps, and the host rate from wall clock. Slot
boundaries are checked by looking for discontinuities in the receive
timestamps: contiguous samples mean the boundaries are exact, because a slot
edge is only a sample count from the anchor. Each stage ends in a PASS or FAIL.

Note that 30.72 MS/s is not a hard requirement for a 20 MHz carrier -- the rate
follows from numerology, FFT size and RB allocation. It is used because it is
the conventional rate for that configuration, and therefore a realistic target.

#### Measured results, 0.5 ms slots, 60 s runs

| stage | 15.36 MS/s | 23.04 MS/s | 30.72 MS/s |
|---|---|---|---|
| `txonly` | - | - | PASS |
| `rxonly` | - | - | PASS (device rate exactly 30.720000) |
| `both` | - | - | FAIL (12-22 underflow) |
| `tdd` | **PASS, PASS** | PASS, FAIL (2 underflow) | 0 / 1 / 5 / 59 / 74 underflow |

Each direction alone sustains 30.72 MS/s comfortably. Both at once is where it
breaks, so the limit is the shared USB 3 pipe and host scheduling rather than
the radio or the slot timing -- note that timestamp gaps were **zero in every
run**, including the failures. Slot boundaries never drifted; the host simply
could not always feed the transmitter.

30.72 MS/s is marginal rather than impossible: one 60 s run was perfectly clean
and others were not, varying by more than an order of magnitude between runs.
15.36 MS/s (10 MHz NR) was clean twice over.

Setting the CPU governor to `performance` and raising `usbfs_memory_mb` to 256
did **not** help -- both were tried and the failures continued, so the usual
first-line tuning is not the answer here.

Buffer sizing did matter, and was a real bug: the transmit buffer was a fixed
sample count, which at 30.72 MS/s is only about 1 ms of data. Sizing it by time
instead (10 ms) turned one 30.72 MS/s run from 74 underflows to zero.

#### Host tuning: what helped and what did not

The streaming threads ask for real-time priority through UHD's helper, which is
in the code and needs no setup. CPU pinning is optional and off by default:

```bash
NR_TX_CPU=2 NR_RX_CPU=3 ./build/tdd_latency_test nr tdd 30.72e6 60
```

Underflows per 60 s run at 30.72 MS/s, across configurations:

| configuration | runs |
|---|---|
| no tuning | 0, 1, 5, 59, 74 |
| **real-time priority only** | **0, 0, 28** |
| isolcpus + nohz_full | 71, 21, 66 |
| isolcpus, cores forced to 3.5 GHz | 0, 65, 37 |
| the above + USB IRQ on an isolated core | 71, 92, 103 |

**CPU isolation made things worse, not better**, and is not recommended here.
Two reasons, both measured:

`nohz_full` stops the scheduler tick on the isolated cores, and intel_pstate
uses that tick to sample utilisation. The isolated cores therefore sat at
400 MHz while cores 0 and 1 ran at 3900 MHz -- even under full load, and even
with the governor set to `performance`. Forcing `scaling_min_freq` up fixed the
frequency but not the underflows.

Moving the USB interrupt onto an isolated core, away from the desktop on CPU 0,
made it worse again: 71, 92, 103.

`isolcpus=2-5` also leaves the operating system only two cores, so it degrades
the untuned case as well. The machine was restored to stock afterwards.

Real-time priority alone remains the best configuration found, and it is
already in the code. It is not sufficient for reliable 30.72 MS/s.

#### Two-radio TDD over the air (stage E)

One NUC transmits the alternating frame, the other measures the envelope:

```bash
# transmitter
./build/tdd_latency_test alt 0.5 60 0.1 80 23.04e6
# receiver
python3 /tmp/slotcheck.py 23.04e6 0.5 50
```

Measured at 23.04 MS/s, 0.5 ms slots, 10 % guard:

| quantity | measured | designed |
|---|---|---|
| duty | 44.9 % | 45 % |
| on-time | 10357 samples = 449.5 us | 10368 = 450 us |
| on/off | 36.6 dB | - |

Slot edges land within 11 samples of design, at a real 15 MHz NR rate.

**Always set the master clock rate.** The B210 divides it to make the sample
rate, and the 32 MHz default turns a request for 23.04 MS/s into 16 and
30.72 MS/s into 32 -- silently, with the wrong rate then measured as though it
were the one asked for. `set_master_clock_for()` handles this; the `alt` mode
was missing it and spent a whole test running at 16 MS/s while reporting 23.04.

#### Reaching 30.72 MS/s: use a narrower wire format

```bash
NR_OTW=sc12 ./build/tdd_latency_test nr tdd 30.72e6 60
```

`sc16` sends four bytes per complex sample, so 30.72 MS/s in both directions is
about 246 MB/s, near 2 Gbps, against perhaps 3.2 Gbps of usable USB 3. Close
enough to the ceiling to explain why it worked only sometimes.

| wire format | bytes/sample | 30.72 MS/s both ways | result over 60 s runs |
|---|---|---|---|
| sc16 | 4 | ~246 MB/s | 5 pass / 12 |
| **sc12** | 3 | ~184 MB/s | **9 pass / 9** |
| sc8 | 2 | ~123 MB/s | 3 pass / 3 |

**sc12 costs nothing in real dynamic range on this device.** The B210's AD9361
has 12-bit converters, so sc12 carries every bit the hardware produces and
sc16 merely pads with zeros. It is 25 % less USB traffic for no loss, and there
is little reason to use sc16 on a B210 at any rate.

Verified over the air at 30.72 MS/s with sc12: duty 44.9 % against 45 %
designed, on-time 13823 samples against 13824, on/off 35.9 dB. Slot edges land
within **one sample**, about 32 ns.

So a 20 MHz-class, 30 kHz SCS NR TDD waveform **is** sustainable here -- the
earlier ceiling was the wire format, not the radio, the host or the slot
timing.

**This needs no real-time kernel.** sc12 at 30.72 MS/s was run nine times on
each kernel and passed every time on both:

| kernel | sc12 @ 30.72 MS/s | sc16 @ 30.72 MS/s |
|---|---|---|
| 7.0.0-31-generic | 9 pass / 9 | 2 pass / 3 |
| 7.0.0-31-realtime | 9 pass / 9 | 5 pass / 12 |

The real-time kernel is therefore not needed, and it has a cost: a SCHED_FIFO
thread at top priority starves the rest of the system, which made ssh unusable
during runs (ping latency went from 0.5 ms to 80 ms and sshd stopped
responding until the run finished). Prefer the generic kernel.

#### PREEMPT_RT kernel

Ubuntu 26.04 carries a real-time kernel in the normal archive -- no Pro
subscription needed:

```bash
sudo apt-get install linux-image-realtime   # then reboot
uname -v                                    # should say PREEMPT_RT
```

The stock kernel already runs `full (lazy)` preemption, so switching the
preemption model gains nothing; only a real PREEMPT_RT build changes anything.

Results, 0.5 ms slots, 60 s runs, on 7.0.0-31-realtime:

| rate | runs | result |
|---|---|---|
| 30.72 MS/s | 12 | **5 pass, 7 fail** (0 to 93 underflows) |
| 23.04 MS/s | 6 | **6 pass**, zero underflows |
| 15.36 MS/s | 6 | **6 pass**, zero underflows |

So the real-time kernel did **not** make 30.72 MS/s reliable. It is about the
same as the stock kernel with real-time priority.

A warning about sampling: the first 9 runs after boot were 9 for 9, including
3 for 3 at 30.72 MS/s, which looked like a clean result. Six more runs at the
same rate gave 5 failures with up to 93 underflows. Three runs is not enough to
tell apart a marginal configuration from a working one here.

Those figures are all for sc16. With sc12 the same 30.72 MS/s passes every
time -- see the wire format section above, which supersedes this as the answer
to whether 20 MHz is reachable.

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
