#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bucket a perf profile by subsystem instead of by symbol.

Per-symbol listings hide cost that is spread over many small functions; this
sums a `perf report` listing into 3D raster / 3D geometry / 2D / JIT code /
JIT runtime / scheduler / memory / IO / SPU / libc buckets, mapping each
symbol to the object file that defines it (nm over the build tree) and the
object to a bucket.

usage:
  perf report -i perf.data --symfs=... --stdio -g none --sort dso,sym --percent-limit 0 > report.txt
  tools/profile_categories.py build/aarch64 report.txt [--detail] [--nm aarch64-linux-gnu-nm]

JIT blocks need DS_PERF_MAP=1 at record time and the /tmp/perf-<pid>.map
file next to perf at report time (see docs/TRACING.md).
"""
import argparse, collections, os, re, subprocess, sys

CATS = [  # (object path prefix under src/core, bucket)
    ('gpu/render3d', '3D raster'), ('gpu/texcache', '3D raster'),
    ('gpu/gpu3d', '3D geometry'),
    ('gpu/engine2d', '2D'), ('gpu/gpu.', '2D'), ('gpu/vram_map', '2D'),
    ('cpu/jit', 'JIT runtime'), ('cpu/cpu', 'JIT runtime'), ('cpu/cp15', 'JIT runtime'),
    ('cpu/interp', 'interpreter'),
    ('mem/', 'memory'), ('nds', 'core loop'), ('sched/', 'scheduler'),
    ('io/', 'IO/DMA/cart'), ('dma/', 'IO/DMA/cart'), ('cart/', 'IO/DMA/cart'),
    ('spu/', 'SPU'), ('profile', 'profile'),
]
KERNEL_3D = re.compile(r'kern::\w+::(depth_candidates|span_)')   # kernels_*.cpp hold both engines' kernels


def symbol_objects(build_dir, nm):
    """symbol -> object path (relative to src/core) for the core objects."""
    sym2obj = {}
    root = os.path.join(build_dir, 'src', 'core')
    for dirpath, _, files in os.walk(root):
        for f in files:
            if not f.endswith('.o'): continue
            path = os.path.join(dirpath, f)
            rel = re.sub(r'.*CMakeFiles/[^/]+/', '', path)[:-2]
            out = subprocess.run([nm, '-C', '--defined-only', path], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split(' ', 2)
                if len(parts) == 3 and parts[1] in 'TtWw': sym2obj.setdefault(parts[2], rel)
    return sym2obj


def bucket(sym2obj, dso, sym):
    if dso.startswith('[kernel') or dso.startswith('[unknown'): return 'kernel'
    if dso.startswith('[JIT]'):
        if re.match(r'jit[79]_', sym): return 'JIT code'
        if sym.startswith('jit_stub'): return 'JIT stubs'
        return 'JIT (unmapped)'
    if 'libc' in dso: return 'libc (memcpy/memset/memcmp)'
    if 'dsperate' not in dso: return 'other: ' + dso
    obj = sym2obj.get(sym)
    if obj is None:
        base = sym.split('(')[0]
        for s, f in sym2obj.items():
            if s.split('(')[0] == base: obj = f; break
    if obj is None: return 'dsperate (unmapped symbol)'
    if obj.startswith('gpu/kernels_'): return '3D raster' if KERNEL_3D.search(sym) else '2D'
    for pre, c in CATS:
        if obj.startswith(pre): return c
    return 'dsperate other: ' + obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir'); ap.add_argument('report')
    ap.add_argument('--detail', action='store_true', help='top symbols per bucket')
    ap.add_argument('--nm', default='aarch64-linux-gnu-nm')
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    sym2obj = symbol_objects(a.build_dir, a.nm)
    tot = collections.Counter(); det = collections.defaultdict(collections.Counter); seen = 0.0
    for line in open(a.report):
        m = re.match(r'\s*([\d.]+)%\s+(.*?)\s+\[[.k]\]\s+(.*)', line)
        if not m: continue
        pct, dso, sym = float(m.group(1)), m.group(2), m.group(3).strip()
        c = bucket(sym2obj, dso, sym); tot[c] += pct; det[c][sym] += pct; seen += pct
    for c, p in tot.most_common():
        print(f'{p:6.2f}%  {c}')
        if a.detail:
            for s, q in det[c].most_common(a.top): print(f'          {q:5.2f}%  {s[:120]}')
    print(f'{seen:6.2f}%  total listed')


if __name__ == '__main__':
    main()
