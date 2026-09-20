#!/usr/bin/env python3
"""Correlate an rx_capture file against a zc_transmit reference.

    ./analyze_capture.py /dev/shm/cap.fc32 /tmp/zc_reference.fc32

Reads the sidecar .txt files for the sample rate and sequence length, so
nothing has to be repeated on the command line.

Both files are raw interleaved complex float32, which is what UHD produces
for "fc32". In numpy that is simply dtype=complex64; in MATLAB:

    f = fopen('cap.fc32','r'); x = fread(f,'float32'); fclose(f);
    x = complex(x(1:2:end), x(2:2:end));
"""
import sys
import numpy as np


def sidecar(path):
    """Read a key=value sidecar, returning {} if there isn't one."""
    meta = {}
    try:
        with open(path + ".txt") as f:
            for line in f:
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    meta[k] = v
    except FileNotFoundError:
        pass
    return meta


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    cap_path, ref_path = sys.argv[1], sys.argv[2]

    cap_meta, ref_meta = sidecar(cap_path), sidecar(ref_path)

    cap = np.fromfile(cap_path, dtype=np.complex64)
    ref = np.fromfile(ref_path, dtype=np.complex64)

    rate = float(cap_meta.get("rate_hz", 30.72e6))

    print(f"  capture   : {len(cap)} samples, {len(cap)/rate*1e3:.1f} ms "
          f"at {rate/1e6:.3f} MS/s")
    print(f"  reference : {len(ref)} samples "
          f"(ZC length {ref_meta.get('zc_length','?')}, "
          f"root {ref_meta.get('zc_root','?')})")

    if cap_meta.get("overflows", "0") != "0":
        print(f"  WARNING: capture reports "
              f"{cap_meta['overflows']} overflows -- it has gaps")

    # Correlate over a couple of hundred repetitions. Eight is enough to find
    # the sequence, but a longer stretch makes the peak statistics steadier.
    win = min(len(cap), 200 * len(ref))
    seg = cap[:win]

    if len(seg) < len(ref):
        print("  capture shorter than the reference")
        return 1

    c = np.abs(np.correlate(seg, ref, mode="valid"))

    peak, mean = c.max(), c.mean()
    print(f"  rms       : {np.sqrt(np.mean(np.abs(seg)**2)):.6f}")
    print(f"  peak/mean : {peak/mean:.1f}")

    if peak / mean < 5:
        print("  NO detection -- that is the noise floor.")
        print("  Check the transmitter was running and the gain is 80.")
        return 1

    # Peak spacing should equal the sequence length, since the transmitter
    # repeats it back to back. That is the real proof it is the ZC and not
    # a startup artefact.
    thr = mean + 0.5 * (peak - mean)
    peaks = np.flatnonzero(c > thr)
    gaps = np.diff(peaks)
    spacing = np.median(gaps[gaps > len(ref) // 2]) if len(gaps) else 0

    print(f"  peaks     : {len(peaks)} above threshold")
    print(f"  spacing   : {spacing:.0f} samples "
          f"(expect {len(ref)}, the sequence length)")
    print(f"  first peak: offset {peaks[0]}")

    ok = abs(spacing - len(ref)) <= 2
    print(f"\n  {'DETECTED, spacing matches the sequence length'
             if ok else 'peaks found but spacing is wrong'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
