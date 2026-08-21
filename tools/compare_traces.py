#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find semantic divergences between two instruction traces (streaming).

  compare_traces.py <reference.trace> <candidate.trace> [--window W] [--confirm C] [--limit N]

Line format: pc instr cpsr r0..r14 (hex). Compares full state per line. When
the traces differ only because of timing (I/O poll loops iterating a different
number of times, interrupts landing a few instructions apart), both sides reach
an identical state again shortly after; on a mismatch the comparator looks up to
W lines ahead on each side for a line that matches exactly and whose following C
lines also match, and resumes there. Only a divergence with no such resync
point is reported as a real bug.
"""
import sys, argparse
from collections import defaultdict, deque

REGS = ['r%d' % i for i in range(15)]

class Stream:
    """Line source with a lookahead buffer and absolute line numbering."""
    def __init__(self, path, limit):
        self.f = open(path); self.buf = deque(); self.base = 0; self.eof = False; self.limit = limit
    def fill(self, n):
        while len(self.buf) < n and not self.eof:
            if self.limit is not None and self.base + len(self.buf) >= self.limit: self.eof = True; break
            l = self.f.readline()
            if not l: self.eof = True; break
            self.buf.append(l.rstrip('\n'))
        return len(self.buf)
    def peek(self, k):
        if self.fill(k + 1) > k: return self.buf[k]
        return None
    def advance(self, n):
        for _ in range(n): self.buf.popleft()
        self.base += n
    def lineno(self): return self.base + 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref'); ap.add_argument('cand')
    ap.add_argument('--context', type=int, default=5)
    ap.add_argument('--window', type=int, default=1000000)
    ap.add_argument('--confirm', type=int, default=8)
    ap.add_argument('--limit', type=int, default=None, help='compare at most N lines of each trace')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--mask-regs', type=int, default=2, help='max registers to mask as dead-value differences (0 disables)')
    a = ap.parse_args()
    R, C = Stream(a.ref, a.limit), Stream(a.cand, a.limit)
    resyncs = skipped_r = skipped_c = 0
    masked = {}        # register index -> line where masking started (dead-value differences)
    mask_events = 0
    hist_r, hist_c = deque(maxlen=a.context), deque(maxlen=a.context)
    def same(lr, lc):
        if lr == lc: return True
        if not masked: return False
        fr, fc = lr.split(), lc.split()
        for k in range(18):
            if k in masked: continue
            if fr[k] != fc[k]: return False
        return True
    while True:
        lr, lc = R.peek(0), C.peek(0)
        if lr is None or lc is None:
            which = 'reference' if lr is None else 'candidate'
            print(f"no semantic divergence: matched to ref line {R.lineno() - 1} / cand line {C.lineno() - 1} ({which} ended first); "
                  f"{resyncs} timing resyncs, skipped {skipped_r} ref / {skipped_c} cand lines; {mask_events} dead-value masks")
            return 0
        if same(lr, lc):
            if masked:     # unmask registers whose values agree again
                fr, fc = lr.split(), lc.split()
                for k in [k for k in masked if fr[k] == fc[k]]: del masked[k]
            hist_r.append(lr); hist_c.append(lc); R.advance(1); C.advance(1); continue
        # Same instruction stream but a register value differs: a value that is
        # (so far) dead. Mask it and carry on; it unmasks when the values agree.
        fr, fc = lr.split(), lc.split()
        if fr[0] == fc[0] and fr[1] == fc[1] and fr[2] == fc[2] and a.mask_regs:
            diff = [k for k in range(3, 18) if fr[k] != fc[k] and k not in masked]
            if 0 < len(diff) <= a.mask_regs and len(masked) + len(diff) <= a.mask_regs:
                # Confirm the instruction stream continues identically for a while.
                ok = True
                for k in range(1, a.confirm + 1):
                    nr, nc = R.peek(k), C.peek(k)
                    if nr is None or nc is None: break
                    pr, pc = nr.split(), nc.split()
                    if pr[0] != pc[0] or pr[1] != pc[1]: ok = False; break
                if ok:
                    for k in diff: masked[k] = R.lineno()
                    mask_events += 1
                    if not a.quiet: print(f"mask: ref {R.lineno()} / cand {C.lineno()} pc {fr[0]}: {', '.join(('r%d' % (k - 3)) for k in diff)} differ ({fr[diff[0]]} vs {fc[diff[0]]}); masked until they agree")
                    hist_r.append(lr); hist_c.append(lc); R.advance(1); C.advance(1); continue
        # Divergence: build candidate window index, scan reference window.
        # A match is a line with identical pc/instr/cpsr and at most
        # `--mask-regs` differing registers (which then get masked), followed
        # by `--confirm` lines with identical pc/instr. Exact matches win.
        nC = C.fill(a.window + a.confirm + 1); nR = R.fill(a.window + a.confirm + 1)
        index = defaultdict(list)
        for jj in range(min(nC, a.window)):
            f = C.buf[jj].split(); index[(f[0], f[1], f[2])].append(jj)
        best = None; best_masked = None
        for ii in range(min(nR, a.window)):
            if best is not None and ii > best[0]: break
            fr = R.buf[ii].split()
            for jj in index.get((fr[0], fr[1], fr[2]), ()):
                if best is not None and ii + jj >= best[0]: break
                fc = C.buf[jj].split()
                diff = [k for k in range(3, 18) if fr[k] != fc[k] and k not in masked]
                if len(diff) > a.mask_regs or len(masked) + len(diff) > a.mask_regs: continue
                ok = True
                for k in range(1, a.confirm + 1):
                    if ii + k >= nR or jj + k >= nC: ok = False; break
                    pr, pc = R.buf[ii + k].split(), C.buf[jj + k].split()
                    if pr[0] != pc[0] or pr[1] != pc[1]: ok = False; break
                    if not diff and R.buf[ii + k] != C.buf[jj + k] and not masked: ok = False; break
                if not ok: continue
                if not diff: best = (ii + jj, ii, jj); break
                if best_masked is None or ii + jj < best_masked[0]: best_masked = (ii + jj, ii, jj, diff)
        if best is None and best_masked is not None:
            cost, ii, jj, diff = best_masked
            for k in diff: masked[k] = R.lineno() + ii
            mask_events += 1
            if not a.quiet: print(f"mask (in resync): {', '.join(('r%d' % (k - 3)) for k in diff)} masked at ref {R.lineno() + ii} / cand {C.lineno() + jj}")
            best = (cost, ii, jj)
        if best is None:
            report(R, C, hist_r, hist_c, resyncs)
            if masked: print("masked registers at this point: " + ', '.join('r%d' % (k - 3) for k in masked))
            return 1
        _, ii, jj = best
        resyncs += 1; skipped_r += ii; skipped_c += jj
        if not a.quiet:
            print(f"resync #{resyncs}: ref {R.lineno()} / cand {C.lineno()} (pc {lr.split()[0]} / {lc.split()[0]}), skipped {ii} / {jj}")
        R.advance(ii); C.advance(jj)

def report(R, C, hist_r, hist_c, resyncs):
    lr, lc = R.peek(0), C.peek(0)
    pr = [int(x, 16) for x in lr.split()]; pc = [int(x, 16) for x in lc.split()]
    names = ['pc', 'instr', 'cpsr'] + REGS
    diff = [n for k, n in enumerate(names) if pr[k] != pc[k]]
    print(f"DIVERGENCE at ref line {R.lineno()} / cand line {C.lineno()} (after {resyncs} timing resyncs): {', '.join(diff)}")
    print("--- reference");  [print("  " + l) for l in list(hist_r) + [lr]]
    print("--- candidate");  [print("  " + l) for l in list(hist_c) + [lc]]
    print("--- field by field")
    for k, nm in enumerate(names): print(f"  {nm:6s} {pr[k]:08x} {pc[k]:08x}{' <==' if nm in diff else ''}")

if __name__ == '__main__':
    sys.exit(main())
