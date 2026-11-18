#!/usr/bin/env python3
"""Compare raw s16le PCM from our decoder with a reference decoder's: find the alignment
(decoders trim different amounts of encoder delay), then report how far apart they are."""
import sys, numpy as np
ch = int(sys.argv[3]) if len(sys.argv) > 3 else 2
a = np.fromfile(sys.argv[1], dtype='<i2').astype(np.int32).reshape(-1, ch)
b = np.fromfile(sys.argv[2], dtype='<i2').astype(np.int32).reshape(-1, ch)
loud = np.nonzero(np.abs(b[:, 0]) > 500)[0]
start = int(loud[0]) if len(loud) else 0
win = b[start:start + 4096, 0]
best = None
for off in range(-3000, 3001):
    lo = start + off
    if lo < 0 or lo + len(win) > len(a): continue
    s = np.abs(a[lo:lo + len(win), 0] - win).sum()
    if best is None or s < best[0]: best = (s, off)
off = best[1] if best else 0
ia, ib = max(off, 0), max(-off, 0)
n = min(len(a) - ia, len(b) - ib)
d = a[ia:ia + n] - b[ib:ib + n]
sig = float((b[ib:ib + n].astype(np.float64) ** 2).sum())
err = float((d.astype(np.float64) ** 2).sum())
snr = 10 * np.log10(sig / err) if err else float('inf')
ad = np.abs(d)
hist = "  ".join(f"|d|={k}: {100 * np.mean(ad == k):.3f}%" for k in range(3)) + f"  |d|>2: {100 * np.mean(ad > 2):.4f}%"
print(f"offset {off:+d}  {n} frames  max diff {ad.max() if n else 0}  SNR {snr:.1f} dB  {hist}  (lengths {len(a)} vs {len(b)})")
