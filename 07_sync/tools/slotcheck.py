import sys, numpy as np, uhd

rate    = float(sys.argv[1]) if len(sys.argv) > 1 else 23.04e6
slot_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
gain    = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0
freq    = 3515e6

u = uhd.usrp.MultiUSRP("")
u.set_master_clock_rate(rate)
u.set_rx_antenna("RX2", 0)

n_cap = int(rate * 0.04)                      # 40 ms of capture
s = np.asarray(u.recv_num_samps(n_cap, freq, rate, [0], gain)).flatten()
s = s[len(s)//2:]                             # drop startup transient

slot_n = int(rate * slot_ms / 1e3)
w = max(8, slot_n // 64)
env = np.convolve(np.abs(s), np.ones(w)/w, mode='same')

hi, lo = env.max(), env.min()
thr = (hi + lo) / 2
on = env > thr
duty = 100 * on.mean()

edges = np.flatnonzero(np.diff(on.astype(int)) != 0)
print(f"  rate={rate/1e6:.2f} MS/s  slot={slot_ms} ms = {slot_n} samples")
if len(edges) > 4:
    d = np.diff(edges)
    med = np.median(d)
    print(f"  duty          = {duty:5.1f} %   (expect ~45 % for a 10 % guard)")
    print(f"  on-time       = {med:8.1f} samples = {med/rate*1e6:7.1f} us"
          f"   (expect {0.9*slot_n:.0f} = {0.9*slot_ms*1000:.0f} us)")
    print(f"  edges found   = {len(edges)}")
    print(f"  on/off        = {20*np.log10(hi/max(lo,1e-12)):5.1f} dB"
          f"   (on={hi:.5f} off={lo:.5f})")
else:
    print(f"  NO slot pattern: duty={duty:.1f} % env {lo:.5f}..{hi:.5f} -- noise")
