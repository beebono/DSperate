#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Side-by-side sheets and numbers: DSperate's upscaled 3D against the DeSmuME reference.

  upscale_sheet.py PICS REF OUT [--scenes a,b] [--zoom 2]

PICS is tools/upscale_pics.sh's output (NAME.frames, NAME.s2.3d, NAME.s4.3d, NAME.s4.scr,
NAME.fF.gpu); REF holds the reference renderer's 4x layer of each dump as NAME.fF.ref4.3d
(dsperate-research/tools/desmume/ref3d DUMP --scale 4) and, optionally, NAME.fF.ref4ne.3d
rendered with --no-edgemark. Records: (256N)x(192N) words r | g<<8 | b<<16 | a<<24
(RGB666, 5-bit alpha).

For each scene and frame, OUT/NAME.fF.png holds, cropped to the 3D's bounds:
  row 1  DSperate 4x | reference 4x | where they differ (coverage red/blue, colour grey)
  row 2  DSperate 2x | reference 4x box-filtered to 2x
and OUT/summary.txt one line per frame: covered pixels, coverage disagreement,
colour difference over the shared pixels, at 4x and at 2x (against the box-filtered
reference). The comparison takes the no-edge-mark reference when it exists, since
edge marking is DSperate's own (one DS pixel wide).
"""
import argparse, os
import numpy as np
from PIL import Image, ImageDraw


def load(path, scale, rec):
    w, h = 256 * scale, 192 * scale
    m = np.memmap(path, dtype='<u4', mode='r')
    n = m.size // (w * h)
    if rec >= n:
        return None
    return np.array(m[rec * w * h:(rec + 1) * w * h]).reshape(h, w)


def premul(a):
    rgb = np.stack([((a >> (8 * i)) & 63) / 63.0 for i in range(3)], -1)
    al = (((a >> 24) & 31) / 31.0)[..., None]
    return np.concatenate([rgb * al, al], -1)


def box(p, f):
    h, w, c = p.shape
    return p.reshape(h // f, f, w // f, f, c).mean(axis=(1, 3))


def show(p, bg=(0.15, 0.0, 0.25)):
    o = p[..., :3] + (1 - p[..., 3:]) * np.array(bg)
    return (np.clip(o, 0, 1) * 255 + 0.5).astype(np.uint8)


def diff_img(a, b):
    """a, b premultiplied at the same size: coverage only in a red, only in b blue, colour difference grey."""
    ca, cb = a[..., 3] > 0, b[..., 3] > 0
    d = np.abs(a[..., :3] - b[..., :3]).max(-1)
    o = np.zeros(a.shape[:2] + (3,), np.uint8)
    both = ca & cb
    g = (np.clip(d * 4, 0, 1) * 255).astype(np.uint8)
    o[both] = np.stack([g, g, g], -1)[both]
    o[ca & ~cb] = (255, 40, 40)
    o[cb & ~ca] = (60, 120, 255)
    return o


def stats(a, b):
    ca, cb = a[..., 3] > 0, b[..., 3] > 0
    both = ca & cb
    d = np.abs(a[..., :3] - b[..., :3]).max(-1)[both] * 63
    return dict(cov=int(ca.sum()), ref=int(cb.sum()), only_ours=int((ca & ~cb).sum()), only_ref=int((cb & ~ca).sum()),
                mean=float(d.mean()) if d.size else 0.0, over4=int((d > 4).sum()))


def label(img, text):
    im = Image.fromarray(img)
    c = Image.new('RGB', (im.width + 6, im.height + 20), (24, 24, 24))
    c.paste(im, (3, 17))
    ImageDraw.Draw(c).text((4, 3), text, fill=(255, 255, 255))
    return c


def row(tiles):
    w = sum(t.width for t in tiles); h = max(t.height for t in tiles)
    o = Image.new('RGB', (w, h), (24, 24, 24)); x = 0
    for t in tiles:
        o.paste(t, (x, 0)); x += t.width
    return o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pics'); ap.add_argument('ref'); ap.add_argument('out')
    ap.add_argument('--scenes'); ap.add_argument('--zoom', type=int, default=2)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    lines = []
    for fn in sorted(os.listdir(a.pics)):
        if not fn.endswith('.frames'):
            continue
        name = fn[:-7]
        if a.scenes and name not in a.scenes.split(','):
            continue
        frames = [int(x) for x in open(os.path.join(a.pics, fn)).read().strip().split(',')]
        start = 0
        for rec, f in enumerate(frames):
            gpu = [g for g in os.listdir(a.pics) if g.startswith(name + '.f') and g.endswith('.gpu')]
            # NAME.fF.gpu is named by the frame asked for; the record is by the frame it landed on: match by log
            tag = None
            for g in gpu:
                log = os.path.join(a.pics, g[:-4] + '.log')
                if os.path.exists(log) and f' frame ' in open(log).read():
                    import re
                    m = re.search(r'gpu dump: frame (\d+) ', open(log).read())
                    if m:
                        fr = int(m.group(1))
                        s = re.search(r'\(frame (\d+)\)', open(log).read())
                        start = int(s.group(1)) if s else 0
                        if fr - start == f:
                            tag = g[:-4]
            if not tag:
                continue
            ours4 = load(os.path.join(a.pics, name + '.s4.3d'), 4, rec)
            ours2 = load(os.path.join(a.pics, name + '.s2.3d'), 2, rec)
            refp = os.path.join(a.ref, tag + '.ref4ne.3d')
            if not os.path.exists(refp):
                refp = os.path.join(a.ref, tag + '.ref4.3d')
            if ours4 is None or not os.path.exists(refp):
                continue
            ref4 = load(refp, 4, 0)
            o4, r4 = premul(ours4), premul(ref4)
            o2, r2 = premul(ours2), box(r4, 2)
            s4, s2 = stats(o4, r4), stats(o2, r2)
            lines.append(f"{tag:22} frame {f:5}  4x: ours {s4['cov']:7} ref {s4['ref']:7} only-ours {s4['only_ours']:6} only-ref {s4['only_ref']:6} "
                         f"colour mean {s4['mean']:.2f} >4/63 {s4['over4']:6} | 2x: only-ours {s2['only_ours']:5} only-ref {s2['only_ref']:5} colour mean {s2['mean']:.2f}")
            cov = (o4[..., 3] > 0) | (r4[..., 3] > 0)
            if not cov.any():
                continue
            ys, xs = np.where(cov)
            y0, y1, x0, x1 = ys.min() // 8 * 8, min(768, (ys.max() // 8 + 1) * 8), xs.min() // 8 * 8, min(1024, (xs.max() // 8 + 1) * 8)
            if (y1 - y0) * (x1 - x0) > 512 * 384:   # big scenes: the centre half, at full detail
                cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
                y0, y1, x0, x1 = max(0, cy - 192) // 8 * 8, min(768, cy + 192), max(0, cx - 256) // 8 * 8, min(1024, cx + 256)
            z = a.zoom
            up = lambda img, k: np.kron(img, np.ones((k, k, 1), np.uint8))
            t1 = [label(up(show(o4)[y0:y1, x0:x1], z), 'DSperate 4x'), label(up(show(r4)[y0:y1, x0:x1], z), 'DeSmuME 4x'),
                  label(up(diff_img(o4, r4)[y0:y1, x0:x1], z), 'diff: red ours only, blue ref only')]
            y0h, y1h, x0h, x1h = y0 // 2, y1 // 2, x0 // 2, x1 // 2
            t2 = [label(up(show(o2)[y0h:y1h, x0h:x1h], 2 * z), 'DSperate 2x'), label(up(show(r2)[y0h:y1h, x0h:x1h], 2 * z), 'DeSmuME 4x box to 2x'),
                  label(up(diff_img(o2, r2)[y0h:y1h, x0h:x1h], 2 * z), 'diff at 2x')]
            r1, r2i = row(t1), row(t2)
            sheet = Image.new('RGB', (max(r1.width, r2i.width), r1.height + r2i.height), (24, 24, 24))
            sheet.paste(r1, (0, 0)); sheet.paste(r2i, (0, r1.height))
            sheet.save(os.path.join(a.out, tag + '.png'))
    open(os.path.join(a.out, 'summary.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
