#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two --dump-audio streams (raw interleaved s16 stereo, 32768 Hz).

    compare_audio.py ref.pcm cand.pcm [--offset K] [--search N] [--frame 547]

Without --offset the candidate is aligned to the reference by searching
[-N, N] samples for the shift with the fewest mismatches over the first few
seconds (games drift by a few frames against melonDS). Prints mismatching
samples per emulated frame (547 samples) and the first differing positions.
"""
import argparse, struct, sys

def load(path):
    d = open(path, 'rb').read()
    n = len(d) // 4
    return struct.unpack('<%dh' % (n * 2), d[:n * 4])

def mismatches(a, b, shift, start, end):
    m = 0
    for i in range(start, end):
        j = i + shift
        if j < 0 or j * 2 + 1 >= len(b): continue
        if a[i * 2] != b[j * 2] or a[i * 2 + 1] != b[j * 2 + 1]: m += 1
    return m

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref'); ap.add_argument('cand')
    ap.add_argument('--offset', type=int); ap.add_argument('--search', type=int, default=4000)
    ap.add_argument('--frame', type=int, default=547)
    ap.add_argument('--first', type=int, default=10, help='print this many first mismatches')
    a = ap.parse_args()
    ref, cand = load(a.ref), load(a.cand)
    nref, ncand = len(ref) // 2, len(cand) // 2
    print('ref %d samples, cand %d samples' % (nref, ncand))
    shift = a.offset
    if shift is None:
        # Align on the first non-silent stretch of the reference.
        start = next((i for i in range(nref) if ref[i * 2] or ref[i * 2 + 1]), 0)
        end = min(nref, start + 32768 * 3)
        best = None
        for s in range(-a.search, a.search + 1):
            m = mismatches(ref, cand, s, start, end)
            if best is None or m < best[0]: best = (m, s)
            if m == 0: break
        shift = best[1]
        print('offset %d (%d mismatches over the alignment window)' % (shift, best[0]))
    total = 0; shown = 0; per_frame = []
    for f in range((nref + a.frame - 1) // a.frame):
        m = mismatches(ref, cand, shift, f * a.frame, min(nref, (f + 1) * a.frame))
        per_frame.append(m); total += m
        if m and shown < a.first:
            for i in range(f * a.frame, min(nref, (f + 1) * a.frame)):
                j = i + shift
                if 0 <= j < ncand and (ref[i * 2] != cand[j * 2] or ref[i * 2 + 1] != cand[j * 2 + 1]):
                    print('frame %d sample %d: ref (%d,%d) cand (%d,%d)' % (f, i, ref[i * 2], ref[i * 2 + 1], cand[j * 2], cand[j * 2 + 1]))
                    shown += 1
                    if shown >= a.first: break
    bad = [f for f, m in enumerate(per_frame) if m]
    if total:   # how big the differences are: RMS of the reference vs RMS of the difference
        import math
        sq = dq = 0.0; cnt = 0
        for i in range(nref):
            j = i + shift
            if j < 0 or j >= ncand: continue
            for c in (0, 1):
                sq += ref[i * 2 + c] ** 2; dq += (ref[i * 2 + c] - cand[j * 2 + c]) ** 2
            cnt += 2
        rs, rd = math.sqrt(sq / cnt), math.sqrt(dq / cnt)
        print('reference rms %.0f, difference rms %.1f (%.1f dB below)' % (rs, rd, 20 * math.log10(rs / rd) if rd else 99))
    print('%d mismatching samples in %d of %d frames' % (total, len(bad), len(per_frame)))
    if bad: print('first bad frames:', bad[:20])
    sys.exit(1 if total else 0)

if __name__ == '__main__':
    main()
