#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fold a PC-sample profile (dsperate-raster-bench --profile) into functions.

    tools/pc_profile.py BIN PROFILE [--addr2line TOOL] [--top N]

BIN must be the exact binary that was profiled, built with -g (debug info
does not change code generation, so a -g build of the same flags is the same
code). Each sample is attributed three ways:
  innermost -- the function the PC is in after inlining is undone: with LTO
               the kernels live inside stage_line / render_band, and this is
               the view that names them;
  outermost -- the real (non-inlined) function the PC's instructions belong to;
  line      -- the innermost file:line of our own source: frames in compiler
               and library headers (arm_neon.h, <array>, bits/...) are skipped,
               so an intrinsic's samples land on the kernel line that used it.
32-bit ARM: --addr2line arm-linux-gnueabihf-addr2line.
"""
import argparse, collections, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("bin")
ap.add_argument("profile")
ap.add_argument("--addr2line", default="addr2line")
ap.add_argument("--top", type=int, default=30)
a = ap.parse_args()

base = 0
pcs = []
for line in open(a.profile):
    line = line.strip()
    if not line:
        continue
    if line.startswith("base "):
        base = int(line.split()[1], 16)
    else:
        pcs.append(int(line, 16) - base)
if not pcs:
    sys.exit("no samples")
counts = collections.Counter(pcs)
uniq = sorted(counts)

out = subprocess.run([a.addr2line, "-a", "-f", "-i", "-C", "-e", a.bin],
                     input="".join(f"{pc:#x}\n" for pc in uniq), capture_output=True, text=True).stdout.splitlines()
# Output per address: the address line, then (function, file:line) pairs, innermost first.
chains = {}
i = 0
cur = None
while i < len(out):
    l = out[i]
    if l.startswith("0x") and (i + 1 >= len(out) or not out[i + 1].startswith("0x")) and len(l) > 2 and all(c in "0123456789abcdefx" for c in l):
        cur = int(l, 16)
        chains[cur] = []
        i += 1
        continue
    if cur is not None and i + 1 < len(out):
        chains[cur].append((l, out[i + 1]))
        i += 2
    else:
        i += 1

LIB_HEADERS = ("arm_neon.h", "/include/c++/", "/bits/", "/array:", "stl_", "/lib/gcc/")
total = len(pcs)
inner, outer, lines = collections.Counter(), collections.Counter(), collections.Counter()
for pc, n in counts.items():
    ch = chains.get(pc) or [("??", "??:0")]
    inner[ch[0][0]] += n
    outer[ch[-1][0]] += n
    own = next((f for f in ch if not any(h in f[1] for h in LIB_HEADERS)), ch[-1])
    loc = own[1].split(" (discriminator")[0].rsplit("/", 1)[-1]
    lines[f"{loc}  {own[0][:60]}"] += n

def show(title, c):
    print(f"== {title} ({total} samples)")
    for k, n in c.most_common(a.top):
        print(f"{100.0 * n / total:6.2f}%  {n:6d}  {k[:110]}")

show("innermost function", inner)
show("outermost function", outer)
show("source line", lines)
