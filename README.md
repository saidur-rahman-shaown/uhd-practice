# UHD practice — USRP B210 experiments

Timing, metadata and synchronisation experiments on two Intel NUCs, each with a
USRP B210, cabled TX/RX to RX2 in both directions with attenuation in line.

**Read [NOTES.md](NOTES.md) before trusting any measurement.** It records every
trap we hit, several of which fail *silently* — the API reports success while
the radio does nothing. It also holds the measured results and the discipline
needed to reproduce them.

## Layout

| directory | subject |
|---|---|
| [`common/`](common/) | shared setup: `Config`, `create_usrp`, the test waveform, async decoders, master clock and buffer helpers |
| [`01_device/`](01_device/) | what is attached and what it can do — run this first |
| [`02_rx/`](02_rx/) | receiving samples |
| [`03_tx/`](03_tx/) | transmitting samples |
| [`04_metadata/`](04_metadata/) | what the device reports back, and the `U`/`L`/`O` status characters |
| [`05_timed_commands/`](05_timed_commands/) | host call timing, scheduling lead time, timed bursts |
| [`06_tdd/`](06_tdd/) | TDD frames, slot cadence, and what rate the link sustains |
| [`07_sync/`](07_sync/) | Zadoff-Chu generation and detection, capture tools for offline analysis |

## Build

One build from the top level produces every program:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

Binaries land in `build/<lesson>/`.

## Where to start

```bash
./build/01_device/device_info            # is the radio there at all
./build/07_sync/zc_sync zcseg            # correlator self-test, no radio needed
./build/04_metadata/async_events all     # provokes each status character in turn
```

`async_events late` is the one to read carefully: `send()` reports success for
ten bursts the device dropped. That gap is the single most important thing to
understand about UHD.

## Settings that matter

| setting | value | why |
|---|---|---|
| wire format | `sc12` (default) | free on a 12-bit converter, and `sc16` will not sustain 30.72 MS/s |
| master clock | equal to the sample rate | otherwise the B210 silently gives you a different rate |
| TX buffer | sized by time, ~10 ms | a fixed sample count shrinks as the rate rises |
| TX gain | 80 | lower is undetectable through the attenuator |
| slot timing | one stream, structure in the samples | per-slot bursts lose half their slots |

All five are explained in [NOTES.md](NOTES.md) §3.

## Hosts

| host | ssh | serial | name |
|---|---|---|---|
| grid3 | `shaown@192.168.0.102` | 310733D | grid3 |
| MyB210 | `shaown@192.168.0.103` | 310731F | MyB210 |
