#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare raw framebuffer dumps (--dump-frames) from two emulators.

Each dump is a sequence of frames; a frame is the top screen then the bottom
screen, 256x192 pixels of little-endian 0xAARRGGBB. Prints per-frame mismatch
counts and, with --png DIR, writes ref/cand/diff PNGs for mismatching frames.

  compare_frames.py ref.bin cand.bin [--png out/] [--max-png N] [--frames A-B] [--ignore-alpha] [--offset K]

--offset K compares candidate frame N against reference frame N+K (K may be
negative), which separates a constant timing skew from rendering differences.
"""
import argparse, os, struct, sys, zlib

W, H = 256, 192
SCREEN = W * H * 4
FRAME = SCREEN * 2

def write_png(path, pixels, scale=1):
    """pixels: bytes of 0xAARRGGBB little-endian (B,G,R,A byte order), W*H.

    scale magnifies by nearest neighbour, which matters when the difference is
    a single pixel: at 1:1 it is not findable by eye.
    """
    rows = bytearray()
    for y in range(H):
        row = pixels[y * W * 4:(y + 1) * W * 4]
        line = bytearray()
        for x in range(W):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            line += bytes((r, g, b)) * scale
        for _ in range(scale):
            rows.append(0)
            rows += line
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', W * scale, H * scale, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(rows), 6)))
        f.write(chunk(b'IEND', b''))

def diff_image(a, b, locator=True):
    """Solid red where the two differ, dimmed greyscale reference elsewhere.

    With `locator`, a red box is drawn around the bounding box of the
    differences (inset by two pixels so it never covers them). A handful of
    changed pixels in a 256x192 frame cannot be found without it.
    """
    out = bytearray(len(a))
    xs, ys = [], []
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            out[i:i + 4] = b'\x00\x00\xff\xff'     # red where different
            p = i // 4; xs.append(p % W); ys.append(p // W)
        else:
            g = (a[i] + a[i + 1] + a[i + 2]) // 12  # dimmed reference elsewhere
            out[i:i + 4] = bytes((g, g, g, 0xff))
    if locator and xs:
        x0, x1 = max(0, min(xs) - 3), min(W - 1, max(xs) + 3)
        y0, y1 = max(0, min(ys) - 3), min(H - 1, max(ys) + 3)
        def put(x, y):
            if 0 <= x < W and 0 <= y < H:
                i = (y * W + x) * 4
                out[i:i + 4] = b'\x00\x00\x80\xff'   # darker red for the box
        for x in range(x0, x1 + 1): put(x, y0); put(x, y1)
        for y in range(y0, y1 + 1): put(x0, y); put(x1, y)
    return bytes(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref'); ap.add_argument('cand')
    ap.add_argument('--png'); ap.add_argument('--max-png', type=int, default=4)
    ap.add_argument('--scale', type=int, default=1, help='magnify the PNGs by nearest neighbour')
    ap.add_argument('--no-locator', action='store_true', help='omit the box drawn around the differing region')
    ap.add_argument('--frames', help='range A-B (inclusive) to compare')
    ap.add_argument('--ignore-alpha', action='store_true')
    ap.add_argument('--offset', type=int, default=0, help='compare cand frame N with ref frame N+offset')
    args = ap.parse_args()
    ref = open(args.ref, 'rb').read(); cand = open(args.cand, 'rb').read()
    nref, ncand = len(ref) // FRAME, len(cand) // FRAME
    n = min(nref - args.offset, ncand) if args.offset >= 0 else min(nref, ncand + args.offset)
    lo, hi = max(0, -args.offset), n - 1
    if args.frames:
        a, _, b = args.frames.partition('-'); lo = int(a); hi = int(b) if b else lo
    if args.png: os.makedirs(args.png, exist_ok=True)
    total_bad = 0; pngs = 0
    for f in range(lo, min(hi, n - 1) + 1):
        rf = f + args.offset
        fr = ref[rf * FRAME:(rf + 1) * FRAME]; fc = cand[f * FRAME:(f + 1) * FRAME]
        counts = []
        for s in range(2):
            a = fr[s * SCREEN:(s + 1) * SCREEN]; b = fc[s * SCREEN:(s + 1) * SCREEN]
            if args.ignore_alpha:
                a = bytes(x if i % 4 != 3 else 0 for i, x in enumerate(a)); b = bytes(x if i % 4 != 3 else 0 for i, x in enumerate(b))
            bad = 0 if a == b else sum(1 for i in range(0, SCREEN, 4) if a[i:i + 4] != b[i:i + 4])
            counts.append(bad)
        if any(counts):
            total_bad += 1
            first = None
            for s in range(2):
                a = fr[s * SCREEN:(s + 1) * SCREEN]; b = fc[s * SCREEN:(s + 1) * SCREEN]
                for i in range(0, SCREEN, 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        p = i // 4
                        first = (('top', 'bottom')[s], p % W, p // W, a[i:i + 4][::-1].hex(), b[i:i + 4][::-1].hex()); break
                if first: break
            print(f'frame {f}: top {counts[0]} bottom {counts[1]} pixels differ; first {first[0]} ({first[1]},{first[2]}) ref {first[3]} cand {first[4]}')
            if args.png and pngs < args.max_png:
                for s, name in enumerate(('top', 'bottom')):
                    if not counts[s]: continue
                    a = fr[s * SCREEN:(s + 1) * SCREEN]; b = fc[s * SCREEN:(s + 1) * SCREEN]
                    write_png(os.path.join(args.png, f'f{f:04d}_{name}_ref.png'), a, args.scale)
                    write_png(os.path.join(args.png, f'f{f:04d}_{name}_cand.png'), b, args.scale)
                    write_png(os.path.join(args.png, f'f{f:04d}_{name}_diff.png'),
                              diff_image(a, b, not args.no_locator), args.scale)
                pngs += 1
    print(f'{min(hi, n - 1) + 1 - lo} frames compared, {total_bad} differ (ref {nref} frames, cand {ncand}, offset {args.offset})')
    sys.exit(1 if total_bad else 0)

if __name__ == '__main__':
    main()
