# UHD B210 experiments — what we built and what bit us

Field notes from building a set of timing experiments on two Intel NUCs, each
with a USRP B210, cabled TX/RX to RX2 in both directions with attenuation in
line.

Most of the value here is in the second half. The programs are small; the traps
were not, and several of them fail *silently* — the API reports success and the
radio does nothing.

---

## 1. The programs

Everything lives in `01_device/`. One `cmake --build build` produces all three.

### `latency_test` — host and device timing

| mode | what it measures |
|---|---|
| `send` | how long `send()` blocks on the host |
| `recv` | how long `recv()` blocks on the host |
| `lead` | minimum scheduling lead time, judged by the device's own reports |
| `txmeta` | TX async events: `BURST_ACK`, `UNDERFLOW`, `TIME_ERROR` |
| `rxmeta` | RX errors: deliberately provokes `OVERFLOW` and `LATE_COMMAND` |
| `zc` | Zadoff-Chu generate and detect, one box (offline self-test plus live) |
| `zctx` / `zcrx` | Zadoff-Chu across two boxes: sender and detector |
| `zcseg` | offline self-test of the segmented correlator |
| `zcdump` | writes one live capture window to disk for offline comparison |
| `txrx` | **broken**, see §3.9 |

### `tdd_latency_test` — TDD slot timing

| mode | what it does |
|---|---|
| `slots` | one self-contained burst per slot. **Fails by design** — see §3.4 |
| `stream` | one continuous burst, slot structure in the samples. Works |
| `alt` | alternating TX-RX-TX-RX frame, both directions at once |
| `nr <stage>` | staged feasibility test: `txonly`, `rxonly`, `both`, `tdd`, `all` |

### `async_events` — a guided tour of UHD's status characters

UHD prints one character straight to stderr per problem, inline and with no
newline: `U` underflow, `L` late packet, `S` sequence error, `O` overflow, `D`
dropped. Silence is the healthy case, which makes them hard to learn from. Each
mode provokes one on demand and prints the decoded message underneath.

### `tools/zc_tx.py` — reference transmitter

A known-good Python sender of the same Zadoff-Chu waveform. Its real job is
diagnostic: when the C++ side looks dead, this tells you in thirty seconds
whether the problem is the radio or the code.

---

## 2. Key numbers we measured

All at 3515 MHz, over the cable, 60 s runs unless noted.

**Scheduling lead time** (`lead 1.0 50`, async-verified):

| lead | bursts on time |
|---|---|
| 0.2 ms | 2 % (1 of 50) |
| 0.5 ms and above | 100 % |

**TDD frame, measured at the far radio:**

| rate | slot | duty | on-time | designed | on/off |
|---|---|---|---|---|---|
| 23.04 MS/s | 0.5 ms | 44.9 % | 449.5 us | 450 us | 36.6 dB |
| 30.72 MS/s | 0.5 ms | 44.9 % | 13823 samples | 13824 | 35.9 dB |

Slot edges land within one sample at 30.72 MS/s — about 32 ns.

**Sustained 30.72 MS/s, both directions, 0.5 ms slots:**

| wire format | bytes/sample | throughput | generic kernel | RT kernel |
|---|---|---|---|---|
| sc16 | 4 | ~246 MB/s | 2 pass / 3 | 5 pass / 12 |
| **sc12** | 3 | ~184 MB/s | **9 pass / 9** | **9 pass / 9** |
| sc8 | 2 | ~123 MB/s | — | 3 pass / 3 |

**Zadoff-Chu detection over the cable:** 2941 of 2941 windows detected,
peak-to-mean 50–91, carrier offset stable at 462–467 Hz between the two
free-running TCXOs.

---

## 3. What bit us

### 3.1 `send()` returning success means nothing

This is the single most important thing on this page.

`tx_stream->send()` returning the full sample count means the *host* handed the
samples to UHD. It does not mean the device transmitted them. A burst whose
`time_spec` has already passed is accepted, then dropped, and reported later
out of band as `EVENT_CODE_TIME_ERROR`.

The original `lead` sweep printed "send accepted" for every lead time down to
0.1 ms while UHD was printing `L` characters for late packets. It looked like a
pass at every setting. `async_events late` shows the gap directly: `send()`
reports success for all ten bursts while the device reports `TIME_ERROR` for
all ten.

**Only the async channel knows what went on air.** Drain it with
`recv_async_msg()` and judge on that.

### 3.2 The B210 silently gives you a different sample rate

The B210 derives its sample rate by dividing a master clock that defaults to
32 MHz. NR rates are not divisors of 32 MHz, so:

- asking for **30.72 MS/s** gives you **32 MS/s**
- asking for **23.04 MS/s** gives you **16 MS/s**

No error, no warning. `get_tx_rate()` returns the wrong-but-real rate, and if
you never read it back you will measure the wrong configuration and believe it
was the one you asked for. This bit us twice, in two different code paths.

**Always call `set_master_clock_rate()` to match, and always read the rate
back.** `set_master_clock_for()` in `tdd_latency_test.cpp` does this.

### 3.3 The stream-startup transient fakes a detection

The first window or two after any stream start — including every
`recv_num_samps()` call, which issues its own stream command — contains a large
broadband transient that correlates against anything.

It produced convincing Zadoff-Chu "detections" with peak-to-mean 7 to 23 that
were pure artefact. The tell was the offsets: always 0 to 12, i.e. always at
the very start of the capture, where real bursts would have appeared at the
burst spacing. Correlation peak height alone did not reveal it.

**Discard roughly the first 100k samples before judging whether a signal is
present.**

### 3.4 One burst per slot does not work at a 1 ms cadence

The obvious way to build a TDD frame — each slot a self-contained burst with
start- and end-of-burst — loses almost exactly half its slots:

| configuration | slots acknowledged |
|---|---|
| 1 ms slots, 20 ms pipeline | 50 % |
| 1 ms slots, 50 ms pipeline | 50 % |
| 2 ms slots, 20 ms pipeline | 50 % |
| 10 ms slots | 100 % |

Neither a deeper pipeline nor a longer slot helps. The device cannot tear down
and re-arm the transmit chain every slot.

**Open the burst once, timestamp only the first sample, and put the slot
structure in the samples** — signal during transmitting slots, zeros during
listening slots. Slot edges then become sample counts from a single anchor and
cannot drift. Measured over 10016 consecutive 1 ms slots: no underflows, no
late packets. `tdd_latency_test slots` is kept precisely so you can watch the
wrong approach fail.

### 3.5 Buffer sizing, measured in time rather than samples

The transmit buffer was a fixed sample count. At 30.72 MS/s that is about 1 ms
of data — far too little to ride out host jitter. Sizing it by time instead
(10 ms) took one run from 74 underflows to zero.

Feeding the device one 1604-sample burst per call also starves it. Batch
several repetitions into one send.

### 3.6 The radio sometimes goes silent, and says nothing

The B210 can enter a state where **nothing is transmitted at all**, and it
survives restarting the program:

- `send()` returns full counts and paces correctly at the sample rate
- `get_tx_freq()`, `get_tx_gain()`, `get_tx_rate()` all read back correctly
- the `lo_locked` sensor reads true
- the far end sits at the noise floor

Nothing anywhere reports an error. Identical binaries that were silent for a
long stretch later reached 82 dB SNR with no code change, which is how several
confident "fixes" were found and then disproved: streamer/tune ordering,
`tune_request_t(freq)` versus `tune_request_t(freq, lo_offset)`, explicit
stream channels, the buffer-pointer form passed to `send()`, a first-packet
time spec, a settling delay, RX configuration, send buffer size. **Every one of
those appeared to fix it and none of them did.**

Root cause never found. Killing processes mid-stream was tested as a trigger
and disproved. If it happens: **power-cycle the B210 before touching code**,
and use `tools/zc_tx.py` to tell a dead radio from a broken program.

### 3.7 sc16 is why 30.72 MS/s was unreliable

`sc16` sends four bytes per complex sample, so 30.72 MS/s in both directions is
about 246 MB/s — near 2 Gbps against perhaps 3.2 Gbps of usable USB 3. Close
enough to the ceiling that it passed only sometimes.

`sc12` costs nothing on this hardware. The B210's AD9361 has **12-bit
converters**, so `sc12` carries every bit the radio produces and `sc16` merely
pads with zeros. 25 % less USB traffic for no loss in dynamic range.

**There is little reason to use `sc16` on a B210 at any rate.**

### 3.8 Host tuning that did not help

Worth recording so it is not retried:

- **CPU isolation made it worse.** `nohz_full` stops the scheduler tick that
  `intel_pstate` uses to sample utilisation, so the isolated cores sat at
  **400 MHz** under full load while the others ran at 3900 — even with the
  `performance` governor. Forcing `scaling_min_freq` up fixed the clock but not
  the underflows. `isolcpus=2-5` also leaves the OS two cores, degrading the
  untuned case.
- **Moving the USB interrupt to an isolated core** was worse again.
- **The PREEMPT_RT kernel changed nothing.** 9 of 9 with sc12 on both kernels;
  sc16 stayed marginal on both. The stock kernel already runs `full (lazy)`
  preemption, so switching preemption models achieves nothing on its own.
- **Real-time priority starves the box.** A `SCHED_FIFO` thread at top priority
  made ping latency go from 0.5 ms to 80 ms with sshd unresponsive until the
  run finished. This is almost certainly what looked earlier like a machine
  "losing" its ssh daemon under load.

### 3.9 `txrx` is broken

`measure_tx_rx_timestamp_latency` reports **negative** latency. It compares the
RX window's start time against the scheduled TX time, and the window is opened
before TX by construction, so the answer is pinned negative regardless of what
the hardware does. It also never drains the previous iteration's stream, so
later commands arrive already expired.

Recovering the real arrival time needs correlation against a known waveform —
which is what the `zc` path does. Use that instead.

### 3.10 Measurement discipline

Most of the wrong conclusions above came from sloppy measurement rather than
from the hardware.

1. Kill every transmitter and **confirm zero survivors**.
2. Measure a baseline and check it is at the noise floor.
3. Only then start the program under test, and **confirm it is alive**.
4. When something starts working, **re-test the broken version immediately**.
   If it also works, your change was not the fix.

A strong tone at a frequency your program does not generate means a stale
transmitter is still running. Three runs is not enough to tell a marginal
configuration from a working one — one sequence of nine consecutive passes was
followed by five failures at the same setting.

Two shell traps: `pkill -f foo` matches the ssh shell running it and kills your
session — use `pkill -f "[f]oo"`. And background remote work with
`setsid nohup … </dev/null &`, or it dies when ssh returns.

### 3.11 USB

One B210 stopped enumerating entirely: `error -110` descriptor timeouts,
`device not accepting address, error -62`, and negotiation at USB 2.0
high-speed rather than SuperSpeed. A physical unplug and replug fixed it.
Confirm SuperSpeed before trusting throughput results:

```bash
for d in /sys/bus/usb/devices/*/; do
  [ "$(cat $d/idVendor 2>/dev/null)" = "2500" ] && echo "speed=$(cat $d/speed)M"
done      # want 5000M
```

---

## 4. How to test it

### Build

```bash
ssh shaown@192.168.0.102          # or .103
cd ~/uhd-practice/01_device
git pull && cmake --build build
```

### One box, no RF needed

Safe anywhere, nothing depends on the cable.

```bash
./build/latency_test zcseg      # correlator self-test, pure maths
./build/async_events all        # provokes U, L, O and LATE_COMMAND in turn
./build/latency_test rxmeta     # forces OVERFLOW and LATE_COMMAND
./build/latency_test lead 1.0 50
```

`async_events` is the one to read carefully — `late` shows `send()` reporting
success for ten bursts the device dropped.

### One box, TDD timing

```bash
./build/tdd_latency_test stream 1.0 10 0.5     # works
./build/tdd_latency_test slots  1.0 1000 20    # fails on purpose, 50 %
NR_OTW=sc12 ./build/tdd_latency_test nr all 30.72e6 60
```

`nr all` runs transmit alone, receive alone, both together, then both with the
slot structure — so a failure at 30.72 MS/s can be attributed to a specific
stage. Each stage ends in PASS or FAIL. **Use `NR_OTW=sc12`.**

### Two boxes over the cable

Sender on one, receiver on the other. **TX gain 80** — 60 and below is too weak
through the attenuator.

```bash
# on .102
cd ~/uhd-practice/01_device
NR_OTW=sc12 ./build/tdd_latency_test alt 0.5 60 0.1 80 30.72e6

# on .103, while that runs
python3 /tmp/slotcheck.py 30.72e6 0.5 50
```

Healthy output is duty near 45 %, on-time near 450 us, on/off above 30 dB.

For Zadoff-Chu detection:

```bash
# sender
./build/latency_test zctx 40 80
# receiver
./build/latency_test zcrx 15 4 4.0 50
```

Healthy output is peak-to-mean 50–110 with offsets scattered across the window
and a steady carrier offset. Peak-to-mean stuck near 2.0 means nothing is
arriving.

### Driving both from a laptop

```bash
ssh shaown@192.168.0.102 'cd ~/uhd-practice/01_device; \
  setsid nohup env NR_OTW=sc12 ./build/tdd_latency_test alt 0.5 60 0.1 80 30.72e6 \
  >/tmp/tx.log 2>&1 </dev/null &'
sleep 10
ssh shaown@192.168.0.103 'python3 /tmp/slotcheck.py 30.72e6 0.5 50'
```

The `setsid nohup … </dev/null &` matters. Without it the sender dies when ssh
returns, and you measure noise while believing you are measuring a transmitter.

### When nothing is received

Measure before changing code.

| rms at the receiver | meaning |
|---|---|
| ~0.00061 | noise floor, nothing arriving |
| ~0.028 | Zadoff-Chu burst arriving normally |
| ~0.060 | CW tone arriving normally |

If a transmitter is running and you still see the noise floor, the radio is in
the silent state of §3.6. Power-cycle it. `tools/zc_tx.py 40 80` is the
reference sender for telling hardware from software.

---

## 5. Settings that matter

| setting | value | why |
|---|---|---|
| wire format | `sc12` | §3.7 — free on a 12-bit converter |
| master clock | equal to the sample rate | §3.2 — or you get a different rate |
| TX buffer | sized by time, ~10 ms | §3.5 |
| TX gain | 80 | lower is undetectable through the attenuator |
| slot timing | one stream, structure in samples | §3.4 |
| kernel | stock | §3.8 — real-time buys nothing here |
