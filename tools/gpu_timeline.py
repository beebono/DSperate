#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Who had the GPU: reads a DS_GPU_TIMELINE log (core/gpu/gpu_timeline.h) and the run's DS_FRAME_SERIES.

  tools/gpu_timeline.py RUN.tl RUN.ser [--name N] [--header] [--dump MS]

The G52 runs its jobs one after another, so a job's time on the GPU is taken as from the later of its own
first timestamp and the end of the job before it, to its last timestamp ("excl"). One line a run:
  fps      emulated frames a second of wall time (59.8 = full speed); 3D: raster frames a second
  busy     the share of the run the GPU had a job
  R, V     the raster's and its vertex stage's ms a job
  P0, P1   each present's ms a job (the SDL frontend opens one a window)
  sum/f    GPU ms a displayed frame: everything above over the frames shown
  wait     a present's ms from submit to done, median and 90th centile; behind: the share submitted while the
           raster had the GPU
  late     per window, frames that reached the compositor more than 1.5 refreshes after the one before
"""
import argparse, bisect
ap = argparse.ArgumentParser(); ap.add_argument('tl'); ap.add_argument('ser'); ap.add_argument('--name', default=''); ap.add_argument('--header', action='store_true')
ap.add_argument('--dump', type=int, default=0, help='print this many ms of events from the middle of the run')
a = ap.parse_args()
PERIOD = 1e9 / 59.8261
ser = [l.split() for l in open(a.ser) if l.strip()]
t_lo, t_hi = int(ser[0][2]), int(ser[-1][3]); T = t_hi - t_lo
ev = []
for l in open(a.tl):
    e = l.split()
    if len(e) >= 4: ev.append((e[0], int(e[1]), int(e[2]), int(e[3]), int(e[4]) if len(e) > 4 else 0))
# The GPU clock's offset: a job's end is never after the CPU saw its fence, and the vertex stage's and the present's
# waits return as it ends. The smallest (cpu - gpu_end) over the run is the offset to within the wake-up.
off = min(t - y for tag, t, x, y, i in ev if tag in ('P_gpu', 'G_gpu') and y > 0)
jobs = []   # (start, end, kind)
subs = {}; pres = []   # per present: its submits in order; (submit, start, end, id)
commits = {}
for tag, t, x, y, i in ev:
    if tag == 'P_submit': subs.setdefault(i, []).append(t)
    elif tag == 'P_commit': commits.setdefault(i, []).append(t)
    elif tag.endswith('_gpu') and y >= x > 0:
        k = tag[0] if tag[0] != 'P' else f'P{i}'
        jobs.append((x + off, y + off, k))
        if tag == 'P_gpu' and subs.get(i): pres.append((subs[i].pop(0), x + off, y + off, i))
jobs.sort(key=lambda j: j[1])
excl = []; prev = 0
for s, e, k in jobs:
    excl.append((max(s, prev), e, k)); prev = e
inw = [(s, e, k) for s, e, k in excl if e > t_lo and s < t_hi]
kinds = sorted({k for _, _, k in inw})
med = lambda v: sorted(v)[len(v) // 2] if v else 0.0
pct = lambda v, p: sorted(v)[min(len(v) - 1, int(len(v) * p))] if v else 0.0
ms = {k: [(e - s) / 1e6 for s, e, kk in inw if kk == k] for k in kinds}
busy = sum(min(e, t_hi) - max(s, t_lo) for s, e, k in inw) / T
rs = sorted((s, e) for s, e, k in excl if k == 'R')
def raster_on(t):
    i = bisect.bisect_right(rs, (t, 1 << 62)) - 1
    return i >= 0 and rs[i][1] > t
pw = [p for p in pres if t_lo <= p[0] < t_hi]
lat = [(e - sub) / 1e6 for sub, s, e, i in pw]
behind = sum(1 for sub, s, e, i in pw if raster_on(sub))
late = []
for i in sorted(commits):
    c = [t for t in commits[i] if t_lo <= t < t_hi]
    late.append(f"{sum(1 for x, y in zip(c, c[1:]) if (y - x) / PERIOD > 1.5)}/{len(c)}")
n = len(ser); fps = n / (T / 1e9)
over = sum(1 for l in ser if float(l[1]) > 16.715)
if a.header:
    print(f"{'scene':11} {'fps':>5} {'3D':>5} {'busy':>5} {'R':>5} {'V':>4} {'P0':>4} {'P1':>4} {'sum/f':>6} {'wait':>5} {'p90':>5} {'behind':>6} {'late':>19} {'work>16.7':>10}")
g = lambda k: med(ms.get(k, []))
print(f"{a.name:11} {fps:5.1f} {len(ms.get('R', [])) / (T / 1e9):5.1f} {busy * 100:4.0f}% {g('R'):5.1f} {g('G'):4.1f} {g('P0'):4.1f} {g('P1'):4.1f} {busy * T / 1e6 / n:6.1f} {med(lat):5.1f} {pct(lat, 0.9):5.1f} {100 * behind / max(1, len(pw)):5.0f}% {' '.join(late):>19} {over:5d}/{n:<4d}")
if a.dump:
    mid = (t_lo + t_hi) // 2; rows = []
    rows += [(s, f"{k:2} on gpu  {(s - mid) / 1e6:8.2f} .. {(e - mid) / 1e6:8.2f}  ({(e - s) / 1e6:.2f} ms)") for s, e, k in excl if mid <= s < mid + a.dump * 1e6]
    rows += [(t, f"{tag:9}  {(t - mid) / 1e6:8.2f}  {i if tag[0] == 'P' else ''}") for tag, t, x, y, i in ev if tag in ('P_submit', 'P_commit', 'R_submit', 'G_submit') and mid <= t < mid + a.dump * 1e6]
    rows += [(int(l[2]), f"frame      {(int(l[2]) - mid) / 1e6:8.2f}  emu {l[0]} work {l[1]}") for l in ser if mid <= int(l[2]) < mid + a.dump * 1e6]
    for t, s in sorted(rows): print('   ', s)
