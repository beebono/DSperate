#!/usr/bin/env python3
# r3d_diff.py REF.rec GPU.rec [--tol N] [--worst K] [--side FRAME OUT.png [--gain G]] [--first N]
#
# Two `dsperate-headless --dump-3d` streams of one run compared frame by frame: the CPU
# raster's 3D layer record (the reference) against the GPU raster's. A record is 256x192
# words, RGB666 in bits 0-5 / 8-13 / 16-21 and the 5-bit alpha in 24-28. Either file may
# be a FIFO.
#
# A pixel differs when one raster drew it (alpha > 0) and the other did not, or both did
# and a colour channel is more than --tol (default 1) of 63 apart. A differing pixel is
# then compared with the other frame's surroundings, a 3x3 and a 5x5 kernel around it:
#   within   each channel of the GPU's pixel lies between the least and the greatest value
#            the reference has in the kernel (give or take the tolerance), and the
#            reference's pixel likewise within the GPU's kernel. "Drawn" counts as a
#            channel. That is what an edge a pixel over or a texel picked next door looks
#            like: a value the surroundings have, or one between them.
#   outside  it is not: a hole in a surface, a pixel where nothing is near, a colour the
#            surroundings do not have. How far outside (the worst channel, of 63) is its
#            size; a pixel only one raster drew with none drawn around it is 63.
# One thing passes that should not: a one-pixel gap in a line one pixel wide, since both
# the line's colour and the background's are in every kernel there.
# The gate (GPU-RENDER-PLAN phase B): no pixel of a frame the player sees is outside the
# 3x3 kernel by more than --gate (default 9) of 63. Such pixels are "blatant"; the rest is
# reported and not gated. --screens FILE is the reference run's --dump-frames stream: a
# frame whose two composed screens are each one flat colour (a fade while the game loads,
# the 3D layer underneath half set up) is hidden and left out.
# --ref-scale N with --scale N: the reference is itself rendered at the scale (melonDS's compute
# renderer, which walks the DS's own edges and spans at any resolution); pixel against pixel, the
# kernels then of its pixels, so an edge one fine pixel over still passes. --pairs matches frames
# of two runs that reach the same scenes at different frame numbers.
# "Undrawn" and "extra" are the differing pixels only one raster drew, whatever their class.
#
# --side writes four panels at 3x: the reference, the GPU's frame, the plain difference as
# grey (the largest channel difference times --gain; white where only one drew), and the
# pixels outside the 3x3 kernel as grey by how far outside (times --gain) on black.
# --levels writes eight panels at 3x in two rows: the reference, the GPU's frame, the plain
# difference as grey, then the pixels outside the 3x3 kernel by more than the tolerance,
# 4, 8, 16 and 32 of 63 (or --thresholds A,B,C,D,E), each set marked red on the dimmed
# reference (the counts are printed).
import argparse, os, queue, struct, sys, threading, zlib
import numpy as np

W, H = 256, 192
N = W * H * 4

def read_exact(f, n):
    out = bytearray()
    while len(out) < n:
        b = f.read(n - len(out))
        if not b: break
        out += b
    return bytes(out)

# A stream read a frame at a time on its own thread: a run that writes two FIFOs holds the
# tail of one in its buffer while it blocks on the other, so they cannot be read in turn.
class Stream:
    def __init__(self, path, size):
        self.q = queue.Queue(8); self.size = size; self.done = False
        if not os.path.exists(path): sys.exit(f'{path}: no such file')   # an error here, not a wait on the thread
        def run():   # opened on the thread: a FIFO's open waits for its writer, and a run opens its outputs in its own order
            with open(path, 'rb') as f:
                while True:
                    b = read_exact(f, size); self.q.put(b)
                    if len(b) < size: break
        threading.Thread(target=run, daemon=True).start()
    def next(self):
        if self.done: return b''
        b = self.q.get()
        if len(b) < self.size: self.done = True
        return b

def png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))
    def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))

# r, g, b and "drawn" (0 or 63) as one vector a pixel; an undrawn pixel has no colour.
def planes(v):
    drawn = ((v >> 24) & 0x1F) > 0
    p = np.stack([((v >> s) & 63).astype(np.int32) for s in (0, 8, 16)] + [drawn.astype(np.int32) * 63], -1)
    p[~drawn, :3] = 0
    return p, drawn

# Least and greatest value of each channel over the (2r+1)^2 kernel, the border repeated.
def envelope(lo, hi, r):
    h, w = lo.shape[:2]
    ql = np.pad(lo, ((r, r), (r, r), (0, 0)), mode='edge'); qh = np.pad(hi, ((r, r), (r, r), (0, 0)), mode='edge')
    ol = oh = None
    for dy in range(2 * r + 1):
        for dx in range(2 * r + 1):
            wl = ql[dy:dy + h, dx:dx + w]; wh = qh[dy:dy + h, dx:dx + w]
            ol = wl if ol is None else np.minimum(ol, wl); oh = wh if oh is None else np.maximum(oh, wh)
    return ol, oh

def up(x, s): return x if s == 1 else np.repeat(np.repeat(x, s, 0), s, 1)

# `a` the 1x reference, `b` the GPU's frame at s times its size. Everything comes back at the
# GPU's size: a reference pixel stands for its s x s block.
#   differing: a GPU pixel against the reference pixel it lies in.
#   outside (e3 for the 3x3 kernel, e5 for 5x5; in DS pixels): how far the GPU pixel is from what
#     the reference holds in the kernel around its DS pixel, or, if worse, how far that reference
#     pixel is from everything the GPU drew over the same kernel (an object that is not there).
def classify(a, b, tol, s=1):
    H, W = a.shape   # the reference's own size (a reference at the GPU's scale: s is 1 and the kernels are of its pixels)
    pa, da = planes(a); pb, db = planes(b)
    ua = up(pa, s)
    d = np.abs(ua - pb).max(-1)
    diff = d > tol
    blo = pb.reshape(H, s, W, s, 4).min((1, 3)); bhi = pb.reshape(H, s, W, s, 4).max((1, 3))   # what the GPU has in each DS pixel
    out = []
    for r in (1, 2):
        lo, hi = envelope(pa, pa, r)
        e_gpu = np.maximum(np.maximum(up(lo, s) - pb, pb - up(hi, s)), 0).max(-1)
        lo, hi = envelope(blo, bhi, r)
        e_ref = np.maximum(np.maximum(lo - pa, pa - hi), 0).max(-1)
        e = np.maximum(e_gpu, up(e_ref, s)); e[~diff & (up(e_ref, s) <= tol)] = 0
        out.append(e)
    uda = up(da, s)
    return diff | (out[0] > tol), out[0], out[1], uda & ~db, db & ~uda, uda & db, d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref'); ap.add_argument('gpu')
    ap.add_argument('--tol', type=int, default=1)
    ap.add_argument('--worst', type=int, default=5)
    ap.add_argument('--gate', type=int, default=9)
    ap.add_argument('--scale', type=int, default=1, help='the GPU stream is this many times 256x192 (--dump-3d-fine)')
    ap.add_argument('--ref-scale', type=int, default=1, help='the reference stream too (1 or --scale): a reference rendered at the scale, compared pixel for pixel, kernels in its pixels')
    ap.add_argument('--pairs', help='FILE of "REFFRAME GPUFRAME" lines: compare these frames of the two streams (two runs that show the same scenes at different frame numbers)')
    ap.add_argument('--screens', help="the reference run's --dump-frames stream: frames with both screens flat are skipped")
    ap.add_argument('--side', nargs=2, metavar=('FRAME', 'OUT'))
    ap.add_argument('--levels', nargs=2, metavar=('FRAME', 'OUT'))
    ap.add_argument('--thresholds', help='for --levels: up to five thresholds, comma separated (default the tolerance, 4, 8, 16, 32)')
    ap.add_argument('--gain', type=float, default=1.0)
    ap.add_argument('--first', type=int, default=0, help='number of the first dumped frame (for the printed frame numbers)')
    o = ap.parse_args()
    fs = Stream(o.screens, 2 * N) if o.screens else None
    S = o.scale; S2 = S * S; RS = o.ref_scale
    if RS not in (1, S): sys.exit('--ref-scale must be 1 or --scale')
    KS = S // RS   # the GPU's pixels to one reference pixel
    fa, fb = Stream(o.ref, N * RS * RS), Stream(o.gpu, N * S2)
    # Frame pairs: the streams in step, or --pairs (each stream read forward once, a window of frames kept).
    def in_step():
        k = 0
        while True:
            ra, rb = fa.next(), fb.next()
            if len(ra) < N * RS * RS or len(rb) < N * S2: return
            yield k, ra, rb; k += 1
    def paired():
        class Win:
            def __init__(self, st, size): self.st, self.size, self.at, self.keep = st, size, 0, {}
            def get(self, k):
                while self.at <= k:
                    b = self.st.next()
                    if len(b) < self.size: return None
                    self.keep[self.at] = b; self.keep.pop(self.at - 96, None); self.at += 1
                return self.keep.get(k)
        wa, wb = Win(fa, N * RS * RS), Win(fb, N * S2)
        for line in open(o.pairs):
            i, j = map(int, line.split()[:2])
            ra, rb = wa.get(i), wb.get(j)
            if ra is None or rb is None: continue
            yield i, ra, rb
    source = paired() if o.pairs else in_step()
    names = ['differ', 'within 3x3', 'within 5x5 only', 'outside both', 'undrawn', 'extra']
    tot = np.zeros(6, np.int64); frames = with3d = clean = 0; drawn = 0
    hist = np.zeros(64, np.int64); out3 = np.zeros(64, np.int64); rows = []
    hidden = blatant = failing = 0
    Z = 3 if S == 1 else 1   # picture enlargement
    for k, ra, rb in source:
        rs = fs.next() if fs and not o.pairs else b''
        a = np.frombuffer(ra, '<u4').reshape(H * RS, W * RS); b = np.frombuffer(rb, '<u4').reshape(H * S, W * S)
        n = o.first + k; frames += 1
        flat = False
        if fs:
            if len(rs) == 2 * N:
                sc = np.frombuffer(rs, '<u4').reshape(2, -1) & 0xFFFFFF
                flat = all(np.unique(x, return_counts=True)[1].max() > 0.99 * x.size for x in sc)
        if not ((a >> 24) & 0x1F).any() and not ((b >> 24) & 0x1F).any(): continue
        picture = (o.levels and n == int(o.levels[0])) or (o.side and n == int(o.side[0]))
        if flat and not picture: hidden += 1; continue
        with3d += 1
        diff, e3, e5, und, ext, both, d = classify(a, b, o.tol, KS)
        a = up(a, KS)   # for the pictures and counts below: the reference at the GPU's size
        in3 = diff & (e3 <= o.tol); in5 = diff & ~in3 & (e5 <= o.tol); bad = diff & (e5 > o.tol)
        c = np.array([diff.sum(), in3.sum(), in5.sum(), bad.sum(), und.sum(), ext.sum()], np.int64)
        tot += c; drawn += int((((a >> 24) & 0x1F) > 0).sum())
        hist += np.bincount(d[both], minlength=64)[:64]; out3 += np.bincount(e3[diff & ~in3], minlength=64)[:64]
        if not c[0]: clean += 1
        nb = int((e3 > o.gate).sum()); blatant += nb; failing += nb > 0
        rows.append((nb, int(c[3]), n, c))
        if o.levels and n == int(o.levels[0]):
            def pic(v):
                r = np.stack([((v >> s) & 63).astype(np.uint8) * 4 for s in (0, 8, 16)], -1); r[((v >> 24) & 0x1F) == 0] = (24, 24, 24); return r
            panels = [pic(a), pic(b), np.repeat(np.clip(d * (255.0 / 63.0) * o.gain, 0, 255).astype(np.uint8)[:, :, None], 3, 2)]
            counts = []
            for t in [int(x) for x in o.thresholds.split(',')] if o.thresholds else (o.tol, 4, 8, 16, 32):
                k = pic(a) // 3; m = diff & (e3 > t); k[m] = (255, 0, 0); panels.append(k); counts.append(f'over {t}: {m.sum() / S2:.0f}')
            print(f'frame {n}, outside the 3x3 kernel by ' + ', '.join(counts))
            gap = np.full((H * S, 4, 3), 90, np.uint8)
            row = lambda ps: np.concatenate([x for q in ps for x in (q, gap)][:-1], 1)
            while len(panels) % 4: panels.append(np.zeros((H * S, W * S, 3), np.uint8))
            rws = [row(panels[k:k + 4]) for k in range(0, len(panels), 4)]
            png(o.levels[1], np.repeat(np.repeat(np.concatenate([x for q in rws for x in (q, np.full((4, q.shape[1], 3), 90, np.uint8))][:-1], 0), Z, 0), Z, 1))
        if o.side and n == int(o.side[0]):
            def pic(v):
                r = np.stack([((v >> s) & 63).astype(np.uint8) * 4 for s in (0, 8, 16)], -1); r[((v >> 24) & 0x1F) == 0] = (24, 24, 24); return r
            def grey(v): return np.repeat(np.clip(v * (255.0 / 63.0) * o.gain, 0, 255).astype(np.uint8)[:, :, None], 3, 2)
            e = e3.copy(); e[in3] = 0
            gap = np.full((H * S, 4, 3), 90, np.uint8)
            png(o.side[1], np.repeat(np.repeat(np.concatenate([pic(a), gap, pic(b), gap, grey(d), gap, grey(e)], 1), Z, 0), Z, 1))
    if not with3d: print(f'{frames} frames, none with 3D'); return 1
    per = tot / with3d / S2; blatant /= S2; drawn /= S2; out3 = out3 / S2
    if hidden: print(f'{hidden} frames with 3D left out: both screens flat')
    print(f'GATE {"pass" if not blatant else "FAIL"}: {blatant / with3d:.1f} blatant px a frame (outside 3x3 by more than {o.gate}), {failing} of {with3d} frames have any')
    if S > 1: print(f'GPU stream at {S}x, reference at {RS}x: counts are in DS pixels (a GPU pixel is 1/{S2}); kernels are of {"DS" if RS == 1 else "the reference\'s"} pixels')
    print(f'{frames} frames, {with3d} with 3D ({clean} with no differing pixel at tolerance {o.tol}), {drawn / with3d:.0f} px drawn a frame')
    print('per frame: ' + ', '.join(f'{k} {v:.1f}' for k, v in zip(names, per)))
    both_n = hist.sum()
    if both_n:
        cum = np.cumsum(hist) / both_n * 100
        print('colour of pixels both drew: ' + ', '.join(f'within {t}: {cum[t]:.3f}%' for t in (0, 1, 2, 4, 8)) + f', worst {int(np.nonzero(hist)[0].max())}')
    n3 = out3.sum()
    if n3: print(f'outside 3x3, by how far: up to 2: {out3[:3].sum() / with3d:.1f} a frame, 3-8: {out3[3:9].sum() / with3d:.1f}, 9-31: {out3[9:32].sum() / with3d:.1f}, 32 and over: {out3[32:].sum() / with3d:.1f}')
    rows.sort(key=lambda r: r[:3], reverse=True)   # not the counts array: paired runs can list a frame twice
    for big, df, n, c in rows[:o.worst]:
        if c[0]: print(f'  frame {n}: blatant {big / S2:.0f}, ' + ', '.join(f'{k} {v / S2:.0f}' for k, v in zip(names, c)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
