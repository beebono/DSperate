#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fold a PC-sample profile (core/pc_sampler.h) into functions.

    tools/pc_profile.py BIN PROFILE [--addr2line TOOL] [--top N]
                        [--perf-map MAP] [--thread NAME]

PROFILE comes from dsperate-headless --pc-profile or dsperate-raster-bench
--profile. BIN must be the exact binary that was profiled, built with -g
(debug info does not change code generation, so a -g build of the same flags
is the same code). Samples in the binary are attributed three ways:
  innermost -- the function the PC is in after inlining is undone: with LTO
               the kernels live inside stage_line / render_band, and this is
               the view that names them;
  outermost -- the real (non-inlined) function the PC's instructions belong to;
  line      -- the innermost file:line of our own source: frames in compiler
               and library headers (arm_neon.h, <array>, bits/...) are skipped,
               so an intrinsic's samples land on the kernel line that used it.
Samples in a shared library are named [lib.so]. Samples in no object are JIT
code: with --perf-map (the /tmp/perf-<pid>.map a DS_PERF_MAP=1 run wrote; the
profile records the pid) they are named by translated block, jit9_<pc> /
jit7_<pc> / jit_stub_*; without it they are [jit]. The arena is reused after a
flush and the map keeps the last block at an address, as perf does.
--lib-dir DIR: copies of the device's shared libraries (same file names);
samples in them are then named by the nearest exported symbol, [libc.so.6
memcpy], which is enough to tell memcpy from a syscall wrapper.
--thread NAME keeps only the samples of threads whose name starts with NAME.
32-bit ARM: --addr2line arm-linux-gnueabihf-addr2line.
"""
import argparse, bisect, collections, os, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("bin")
ap.add_argument("profile")
ap.add_argument("--addr2line", default="addr2line")
ap.add_argument("--top", type=int, default=30)
ap.add_argument("--perf-map")
ap.add_argument("--thread")
ap.add_argument("--lib-dir")
ap.add_argument("--nm", default="nm")
a = ap.parse_args()

base = 0
objs = []          # (lo, hi, path)
threads = {}       # tid -> name
cpu_ticks = {}     # tid -> CPU clock ticks over the window
window = None      # (seconds, ticks per second)
samples = []       # (absolute pc, tid)
for line in open(a.profile):
    f = line.split()
    if not f:
        continue
    if f[0] == "base":
        base = int(f[1], 16)
    elif f[0] == "pid":
        pass
    elif f[0] == "obj":
        objs.append((int(f[1], 16), int(f[2], 16), " ".join(f[3:])))
    elif f[0] == "window":
        window = (float(f[1]), int(f[2]))
    elif f[0] == "thread":
        if len(f) >= 4 and f[-1].lstrip("-").isdigit():
            threads[int(f[1])] = " ".join(f[2:-1])
            cpu_ticks[int(f[1])] = int(f[-1])
        else:
            threads[int(f[1])] = " ".join(f[2:])
    else:
        samples.append((int(f[0], 16), int(f[1]) if len(f) > 1 else 0))
if not samples:
    sys.exit("no samples")

def tname(tid):
    return f"{threads.get(tid, '?')}/{tid}" if tid else "all"

if a.thread:
    samples = [s for s in samples if threads.get(s[1], "").startswith(a.thread)]
    if not samples:
        sys.exit(f"no samples on threads named {a.thread}*")

objs.sort()
obj_lo = [o[0] for o in objs]
def obj_of(pc):
    i = bisect.bisect_right(obj_lo, pc) - 1
    if i >= 0 and pc < objs[i][1]:
        return objs[i]
    return None
# The executable is the object at the load base (an old profile has no obj lines: every PC is in it).
exe = next((o for o in objs if os.path.basename(o[2]) == os.path.basename(a.bin)), objs[0] if objs else None)

jit = []           # sorted (start, end, name)
if a.perf_map:
    last = {}
    for line in open(a.perf_map):
        f = line.split()
        if len(f) >= 3:
            last[int(f[0], 16)] = (int(f[1], 16), f[2])
    jit = sorted((s, s + n, name) for s, (n, name) in last.items())
jit_lo = [j[0] for j in jit]
def jit_name(pc):
    i = bisect.bisect_right(jit_lo, pc) - 1
    if i >= 0 and pc < jit[i][1]:
        return jit[i][2]
    return None

# Shared-library symbols: nearest exported symbol at or below the offset. The
# first executable segment starts at the file's vaddr 0 for these libraries.
lib_syms = {}
def lib_name(o, pc):
    base_name = os.path.basename(o[2]) or "vdso"
    path = os.path.join(a.lib_dir, base_name) if a.lib_dir else None
    if not path or not os.path.exists(path):
        return f"[{base_name}]"
    if path not in lib_syms:
        syms = []
        for opt in (["-D", "--defined-only"], ["--defined-only"]):
            r = subprocess.run([a.nm] + opt + [path], capture_output=True, text=True).stdout
            for l in r.splitlines():
                f = l.split()
                if len(f) == 3 and f[1] in "TtWw":
                    syms.append((int(f[0], 16) & ~1, f[2]))
        syms.sort()
        lib_syms[path] = ([v for v, _ in syms], [n for _, n in syms])
    vals, names = lib_syms[path]
    i = bisect.bisect_right(vals, pc - o[0]) - 1
    return f"[{base_name} {names[i] if i >= 0 else '?'}]"

# Classify every sample: ("exe", offset) / ("lib", name) / ("jit", name).
kinds = []
for pc, tid in samples:
    o = obj_of(pc) if objs else exe
    if not objs or o is exe:
        kinds.append(("exe", pc - base))
    elif o is not None:
        kinds.append(("lib", lib_name(o, pc)))
    else:
        n = jit_name(pc)
        kinds.append(("jit", n if n else "[jit]"))

offs = sorted({k[1] for k in kinds if k[0] == "exe"})
out = subprocess.run([a.addr2line, "-a", "-f", "-i", "-C", "-e", a.bin],
                     input="".join(f"{pc:#x}\n" for pc in offs), capture_output=True, text=True).stdout.splitlines()
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
total = len(samples)
inner, outer, lines, cats, by_thread = (collections.Counter() for _ in range(5))
thread_outer = collections.defaultdict(collections.Counter)
for (pc, tid), (kind, key) in zip(samples, kinds):
    by_thread[tname(tid)] += 1
    if kind == "exe":
        ch = chains.get(key) or [("??", "??:0")]
        inner[ch[0][0]] += 1
        outer[ch[-1][0]] += 1
        own = next((f for f in ch if not any(h in f[1] for h in LIB_HEADERS)), ch[-1])
        loc = own[1].split(" (discriminator")[0].rsplit("/", 1)[-1]
        lines[f"{loc}  {own[0][:60]}"] += 1
        cats["emulator code"] += 1
        thread_outer[tname(tid)][ch[-1][0]] += 1
    else:
        inner[key] += 1
        outer[key] += 1
        lines[key] += 1
        if kind == "lib":
            cats["shared libraries"] += 1
        elif key.startswith("jit9"):
            cats["JIT blocks, ARM9"] += 1
        elif key.startswith("jit7"):
            cats["JIT blocks, ARM7"] += 1
        elif key.startswith("jit_stub"):
            cats["JIT stubs"] += 1
        else:
            cats["JIT / anonymous code"] += 1
        thread_outer[tname(tid)][key if kind == "lib" or not key.startswith("jit") or key.startswith("jit_stub") else f"[jit{key[3]} blocks]"] += 1

def show(title, c, n=a.top, of=total):
    print(f"== {title} ({of} samples)")
    for k, v in c.most_common(n):
        print(f"{100.0 * v / of:6.2f}%  {v:6d}  {k[:110]}")

if len(by_thread) > 1:
    show("thread", by_thread)
if window and cpu_ticks:
    secs, hz = window
    print(f"== CPU over the {secs:.2f} s window (of {len(threads)} threads alive at the end)")
    tot = 0
    for tid, t in sorted(cpu_ticks.items(), key=lambda kv: -kv[1]):
        if t > 0:
            tot += t
            print(f"{t / hz:8.2f} s  {100.0 * t / hz / secs:6.1f}% of a core  {tname(tid)}")
    print(f"{tot / hz:8.2f} s  {100.0 * tot / hz / secs:6.1f}% of a core  total")
show("kind", cats)
show("innermost function", inner)
show("outermost function", outer)
show("source line", lines)
if len(by_thread) > 1:
    for t, n in by_thread.most_common():
        if n * 20 >= total:
            show(f"outermost function on {t}", thread_outer[t], min(a.top, 15), n)
