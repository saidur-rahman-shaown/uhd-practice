# UHD practice — USRP B210 experiments

Timing, metadata and synchronisation experiments on two Intel NUCs, each with a
USRP B210, cabled TX/RX to RX2 in both directions with attenuation in line.

**Read [NOTES.md](NOTES.md) before trusting any measurement.** It records every
trap we hit, several of which fail *silently* — the API reports success while
the radio does nothing — along with the measured results and the discipline
needed to reproduce them.

## Layout

| directory | subject |
|---|---|
| [`common/`](common/) | shared setup: `Config`, `create_usrp`, test waveform, async decoders, master-clock and buffer helpers |
| [`01_device/`](01_device/) | what is attached and what it can do — run this first |
| [`02_rx/`](02_rx/) | receiving samples |
| [`03_tx/`](03_tx/) | transmitting samples |
| [`04_metadata/`](04_metadata/) | what the device reports back, and the `U`/`L`/`O` status characters |
| [`05_timed_commands/`](05_timed_commands/) | host call timing, scheduling lead time, timed bursts |
| [`06_tdd/`](06_tdd/) | TDD frames, slot cadence, sustainable rate |
| [`07_sync/`](07_sync/) | Zadoff-Chu generation and detection, capture tools for offline analysis |

## Build

One build from the repository root produces every program:

```bash
cd ~/uhd-practice
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

Binaries land in `build/<lesson>/`. The Mac has no UHD — build and run on the
NUCs.

## Quick start

Three commands, in order, on a machine you have not used before:

```bash
./build/01_device/device_info          # is the radio there at all
./build/07_sync/zc_sync zcseg          # correlator self-test, no radio needed
./build/04_metadata/async_events all   # provokes each status character in turn
```

---

# Running the programs

## 01_device — what is attached

```bash
./build/01_device/device_info
```

Prints the device tree, master clock, antennas, frequency and gain ranges,
clock and time sources, and available sensors. Run it first: it proves UHD can
find and open the radio before an experiment blames itself for a USB cable.

## 04_metadata — the status characters

While streaming, UHD writes one character straight to stderr per problem, with
no newline: `U` underflow, `L` late packet, `S` sequence error, `O` overflow,
`D` dropped. Silence is the healthy case, which makes them hard to learn from.

```bash
./build/04_metadata/async_events ack         # clean burst — nothing printed
./build/04_metadata/async_events late        # provokes L and TIME_ERROR
./build/04_metadata/async_events underflow   # provokes U and UNDERFLOW
./build/04_metadata/async_events overflow    # provokes O and ERROR_CODE_OVERFLOW
./build/04_metadata/async_events latecmd     # provokes LATE_COMMAND on receive
./build/04_metadata/async_events all
```

`late` is the one to read carefully:

```
LLLLLLLLLL
  send() reported success : 10 of 10
  TIME_ERROR from device  : 10
```

**`send()` returning success does not mean anything was transmitted.** That gap
is the single most important thing to understand about UHD.

## 05_timed_commands — timing and lead time

```bash
./build/05_timed_commands/latency_test send        # host send() duration
./build/05_timed_commands/latency_test recv        # host recv() duration
./build/05_timed_commands/latency_test lead 1.0 50 # minimum scheduling lead
./build/05_timed_commands/latency_test txmeta      # TX async events
./build/05_timed_commands/latency_test rxmeta      # forces OVERFLOW, LATE_COMMAND
./build/05_timed_commands/latency_test all
```

`lead` sweeps scheduling lead times and judges each burst by the device's own
report rather than by `send()` returning:

```
  lead      ACK   late  under   none   on-time
    0.2 ms      1     49      0      0      2.0 %
    0.5 ms     50      0      0      0    100.0 %
```

`txrx` exists but is **broken** — it reports negative latency. Use the
Zadoff-Chu path in `07_sync` instead. See NOTES §3.9.

## 06_tdd — frames and slot cadence

```bash
# one continuous stream, slot structure in the samples — this works
./build/06_tdd/tdd_latency_test stream 0.5 10 0.5

# one self-contained burst per slot — fails on purpose, ~50 % of slots
./build/06_tdd/tdd_latency_test slots 0.5 1000 20

# alternating TX-RX-TX-RX frame, both directions at once
./build/06_tdd/tdd_latency_test alt 0.5 60 0.1 80 30.72e6

# staged feasibility: transmit alone, receive alone, both, both with slots
./build/06_tdd/tdd_latency_test nr all 30.72e6 60
```

Arguments: `stream <slot_ms> <seconds> <duty>`,
`slots <slot_ms> <slots> <pipeline_ms>`,
`alt <slot_ms> <seconds> <guard> <tx_gain> <rate>`,
`nr <stage> <rate> <seconds> <slot_ms> <guard> <tx_gain> <warmup>`.

`nr` runs in stages so a failure at a given rate can be attributed to one of
them, and each ends in PASS or FAIL:

```
  master clock   = 30.720000 MHz
  wire format    = sc12
  slot           = 0.500 ms = 15360 samples
      underflow   = 0
      overflow    = 0
      timestamp gaps = 0
  VERDICT: PASS  (tdd at 30.720 MS/s)
```

## 07_sync — Zadoff-Chu

```bash
./build/07_sync/zc_sync zcseg          # offline self-test, no radio
./build/07_sync/zc_sync single         # TX and RX on one radio (needs loopback)
./build/07_sync/zc_sync tx 40 80       # two-host sender: seconds, tx_gain
./build/07_sync/zc_sync rx 15 4 4.0 50 # receiver: seconds, segments, threshold, rx_gain
./build/07_sync/zc_sync dump 50        # save one live window to disk
```

`zcseg` needs no hardware and should always pass:

```
  segments =  1 : peak/mean =  181.88, offset =   777  (correct)
  segments = 16 : peak/mean =   37.18, offset =   777  (correct)
```

---

# Two-host workflows

Sender on one NUC, receiver on the other. **Use TX gain 80** — 60 and below is
undetectable through the attenuator.

## Live Zadoff-Chu detection

```bash
# on .102
./build/07_sync/zc_sync tx 40 80

# on .103, while that runs
./build/07_sync/zc_sync rx 15 4 4.0 50
```

Healthy output — peak-to-mean 50–110, offsets scattered across the window, and
a steady carrier offset:

```
  DETECT: offset  1773, peak/mean =   50.2, CFO =     464 Hz
  DETECT: offset  1302, peak/mean =   90.6, CFO =     463 Hz
  detected      = 2941 (100.0 %)
```

Peak-to-mean stuck near 2.0 means nothing is arriving.

## TDD frame, measured on air

```bash
# on .102
./build/06_tdd/tdd_latency_test alt 0.5 60 0.1 80 30.72e6

# on .103
python3 07_sync/tools/slotcheck.py 30.72e6 0.5 50
```

```
  duty          =  44.9 %   (expect ~45 % for a 10 % guard)
  on-time       =  13823.0 samples =   450.0 us   (expect 13824 = 450 us)
  on/off        =  35.9 dB
```

## Capture for offline analysis

```bash
# .102 — transmit, and write the exact reference being sent
./build/07_sync/zc_transmit 45 80 30.72e6 401 25

# .103 — capture one second into RAM
./build/07_sync/rx_capture /dev/shm/cap.fc32 1 30.72e6 3515 50

# bring the reference across
scp shaown@192.168.0.102:/tmp/zc_reference.fc32* /dev/shm/
```

At 30.72 MS/s a capture is **246 MB/s to disk**. Write into `/dev/shm`, which
is RAM, and keep the run short. `rx_capture` reports overflows; a capture with
overflows has gaps in the middle and its timing cannot be trusted.

## Driving both from a laptop

```bash
ssh shaown@192.168.0.102 'cd ~/uhd-practice; \
  setsid nohup ./build/07_sync/zc_transmit 45 80 30.72e6 401 25 \
  >/tmp/tx.log 2>&1 </dev/null &'
sleep 10
ssh shaown@192.168.0.103 'cd ~/uhd-practice && \
  ./build/07_sync/rx_capture /dev/shm/cap.fc32 1 30.72e6 3515 50'
```

The `setsid nohup … </dev/null &` matters. Without it the sender dies when ssh
returns, and you measure noise while believing you are measuring a transmitter.

---

# Offline analysis

Both the capture and the reference are raw **interleaved complex float32** —
UHD's `fc32` host format. Each has a `.txt` sidecar carrying the sample rate,
frequency, gain and sequence parameters, because a capture without its sample
rate is not analysable.

## MATLAB

[`07_sync/tools/analyze_capture.m`](07_sync/tools/analyze_capture.m) does the
whole job and plots the result:

```matlab
cd 07_sync/tools
analyze_capture('/dev/shm/cap.fc32', '/dev/shm/zc_reference.fc32')
```

```
  capture   : 30722400 samples, 1000.1 ms at 30.720 MS/s
  reference : 401 samples (root 25)
  rms       : 0.059006
  peak/mean : 76.6 at offset 348
  peaks     : 14 above threshold
  spacing   : 400 samples (expect 401)
  CFO       : +464.2 Hz (0.13 ppm at 3515 MHz)

  DETECTED, spacing matches the sequence length
```

The pieces, if you would rather do it by hand:

```matlab
% read interleaved complex float32
f = fopen('/dev/shm/cap.fc32','r');
raw = fread(f, Inf, 'float32'); fclose(f);
x = complex(raw(1:2:end), raw(2:2:end));

% the same sequence the transmitter sent (length 401, root 25)
N = 401; u = 25;
n = (0:N-1).';
zc = exp(-1j*pi*u*n.*(n+1)/N);

% matched filter: correlation is conv with the reversed conjugate
c = conv(x(1:8*N), conj(flipud(zc)), 'valid');
m = abs(c);

[peak, idx] = max(m);
fprintf('peak/mean %.1f at lag %d\n', peak/mean(m), idx-1);

% carrier offset from the phase advance between repetitions
fs = 30.72e6;
k = idx + (0:9)*N;
cfo = angle(mean(c(k(2:end)).*conj(c(k(1:end-1))))) / (2*pi*N/fs);
fprintf('CFO %.1f Hz\n', cfo);

plot(m); xlabel('lag (samples)'); ylabel('|correlation|'); grid on
```

**Judge the spacing, not the peak height.** A tall peak at an arbitrary offset
is the stream-startup transient, which correlates against anything and fooled
us repeatedly (NOTES §3.3). A peak every 401 samples is the sequence.

## Python

```bash
./07_sync/tools/analyze_capture.py /dev/shm/cap.fc32 /dev/shm/zc_reference.fc32
```

```python
import numpy as np
x  = np.fromfile("cap.fc32", dtype=np.complex64)
zc = np.fromfile("zc_reference.fc32", dtype=np.complex64)
c  = np.abs(np.correlate(x[:8*len(zc)], zc, mode="valid"))
print(c.max() / c.mean())
```

---

# Settings that matter

| setting | value | why |
|---|---|---|
| wire format | `sc12` (default) | free on a 12-bit converter; `sc16` will not sustain 30.72 MS/s |
| master clock | equal to the sample rate | otherwise the B210 silently gives a different rate |
| TX buffer | sized by time, ~10 ms | a fixed sample count shrinks as the rate rises |
| TX gain | 80 | lower is undetectable through the attenuator |
| slot timing | one stream, structure in samples | per-slot bursts lose half their slots |

All five are explained in [NOTES.md](NOTES.md) §3. Override the wire format for
comparison with `NR_OTW=sc16`.

# When nothing is received

Measure before changing code. On the receiving box:

```bash
python3 07_sync/tools/slotcheck.py 30.72e6 0.5 50
```

| rms | meaning |
|---|---|
| ~0.00061 | noise floor — nothing arriving |
| ~0.028 | Zadoff-Chu burst arriving normally |
| ~0.060 | CW tone arriving normally |

If a transmitter is running and you still see the noise floor, the radio may be
in the silent state of NOTES §3.6 — **power-cycle the B210 before touching
code**. `07_sync/tools/zc_tx.py 40 80` is a known-good Python sender for
telling a dead radio from a broken program.

# Hosts

| host | ssh | serial | name |
|---|---|---|---|
| grid3 | `shaown@192.168.0.102` | 310733D | grid3 |
| MyB210 | `shaown@192.168.0.103` | 310731F | MyB210 |

Both are cabled TX/RX → RX2 to each other with attenuation in line, on
free-running internal clocks (a few hundred Hz of carrier offset between them).
