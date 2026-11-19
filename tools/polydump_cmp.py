#!/usr/bin/env python3
"""Compares two rasters pixel by pixel with the polygon that drew each pixel (DS_R3D_POLYDUMP, tools/polydump.sh).

  polydump_cmp.py REF.id GPU.id REF.3d GPU.3d [--first N] [--frame F [--box X0,Y0,X1,Y1] [--png OUT] [--polys K]]

The .id files are --dump-3d records of runs with DS_R3D_POLYDUMP=1, the .3d files the same runs without it
(the colours); --first is the frame number of the dumps' first frame. Each pixel opaque in both falls in one class:
  missing      the reference drew it, the other raster nothing            (blue in the picture)
  extra        the other raster drew it, the reference nothing            (magenta)
  wrong poly   another polygon on top, and the colour is off by more than --tol of 63   (red)
  wrong poly=  another polygon on top, the colour within --tol            (not drawn: a shared edge, as a rule)
  colour       the same polygon, the colour off by more than --tol        (yellow)
Pixels translucent in either are left out. With --frame: that frame's counts, the polygons that lose or gain
most pixels, and with --png the reference, the other raster and the classes side by side (12x)."""
import argparse, collections, numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("files", nargs=4); ap.add_argument("--first", type=int, default=0); ap.add_argument("--frame", type=int)
ap.add_argument("--box"); ap.add_argument("--png"); ap.add_argument("--polys", type=int, default=12); ap.add_argument("--tol", type=int, default=4)
a = ap.parse_args()
ri, gi, rc, gc = (np.memmap(f, dtype=np.uint32, mode="r").reshape(-1, 192, 256) for f in a.files)

def classes(ri, gi, rc, gc):
    ra, ga = (ri >> 24) & 31, (gi >> 24) & 31
    d = np.zeros(ri.shape, int)
    for s in (0, 8, 16): d = np.maximum(d, np.abs(((rc >> s) & 63).astype(int) - ((gc >> s) & 63).astype(int)))
    both = (ra == 31) & (ga == 31); same = (ri & 0xFFFFFF) == (gi & 0xFFFFFF); off = d > a.tol
    return {"missing": (ra == 31) & (ga == 0), "extra": (ga == 31) & (ra == 0), "wrong poly": both & ~same & off,
            "wrong poly=": both & ~same & ~off, "colour": both & same & off, "same": both & same & ~off}
num = lambda w: (w & 63) | (((w >> 8) & 63) << 6)

if a.frame is None:
    n = len(ri); tot = collections.Counter()
    for f in range(n):
        for k, m in classes(ri[f], gi[f], rc[f], gc[f]).items(): tot[k] += int(m.sum())
    print(f"{n} frames, pixels a frame: " + "  ".join(f"{k} {v / n:.1f}" for k, v in tot.items()))
    raise SystemExit
f = a.frame - a.first
x0, y0, x1, y1 = (int(v) for v in a.box.split(",")) if a.box else (0, 0, 256, 192)
cut = lambda w: np.asarray(w[f])[y0:y1, x0:x1]
RI, GI, RC, GC = cut(ri), cut(gi), cut(rc), cut(gc)
cl = classes(RI, GI, RC, GC)
print(f"frame {a.frame}, x {x0}-{x1} y {y0}-{y1}: " + "  ".join(f"{k} {int(m.sum())}" for k, m in cl.items()))
bad = cl["missing"] | cl["wrong poly"]
rn, gn = num(RI), num(GI)
rows = []
for p in set(rn[(RI >> 24) & 31 == 31].tolist()) | set(gn[(GI >> 24) & 31 == 31].tolist()):
    mine_r, mine_g = (rn == p) & ((RI >> 24) & 31 == 31), (gn == p) & ((GI >> 24) & 31 == 31)
    lost_none = int((mine_r & cl["missing"]).sum()); lost_poly = int((mine_r & cl["wrong poly"]).sum())
    took = int((mine_g & (cl["wrong poly"] | cl["extra"])).sum()); col = int((mine_r & cl["colour"]).sum())
    to = collections.Counter(gn[mine_r & cl["wrong poly"]].tolist()).most_common(3)
    if lost_none + lost_poly + took + col: rows.append((lost_none + lost_poly + took + col, p, int(mine_r.sum()), int(mine_g.sum()), lost_none, lost_poly, to, took, col))
print("polygon  ref px  gpu px  lost: undrawn  to another polygon (which: px)            drawn where it should not  own px, wrong colour")
for _, p, nr, ng, ln, lp, to, tk, col in sorted(rows, reverse=True)[:a.polys]:
    print(f"{p:7d} {nr:7d} {ng:7d} {ln:14d}  {lp:4d} {', '.join(f'{q}: {c}' for q, c in to):36s} {tk:10d} {col:21d}")
if a.png:
    from PIL import Image
    rgb = lambda w: np.dstack([(((w >> s) & 63) * 255 // 63).astype(np.uint8) for s in (0, 8, 16)])
    def idc(w):   # a colour per polygon number
        n = num(w).astype(np.uint32); h = (n * 2654435761) & 0xFFFFFFFF
        o = np.dstack([(h >> s & 0xFF).astype(np.uint8) | 0x40 for s in (0, 8, 16)]); o[(w >> 24) & 31 == 0] = 0; return o
    mark = rgb(RC) // 3
    for k, c in (("colour", (255, 255, 0)), ("wrong poly", (255, 0, 0)), ("missing", (0, 0, 255)), ("extra", (255, 0, 255))): mark[cl[k]] = c
    ims = [Image.fromarray(i).resize((i.shape[1] * 12, i.shape[0] * 12), Image.NEAREST) for i in (rgb(RC), rgb(GC), idc(RI), idc(GI), mark)]
    out = Image.new("RGB", (sum(i.width for i in ims) + 10 * (len(ims) - 1), ims[0].height)); x = 0
    for i in ims: out.paste(i, (x, 0)); x += i.width + 10
    out.save(a.png); print("wrote", a.png, "(reference, other raster, reference's polygons, other's polygons, classes)")
