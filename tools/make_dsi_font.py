#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build DSperate's own DSi system font, /sys/TWLFontTable.dat.

    make_dsi_font.py NotoSans-Regular.ttf wqy-microhei.ttc out.dat

DSiWare that draws text with the console's shared font reads it from the
NAND. With no NAND dump DSperate supplies this one instead: the same table
layout (GBATEK "DSi SD/MMC Firmware Font File"), three Nitro fonts (16x21,
12x16 and 10x12 cells, 2 bits per pixel) compressed with the DSi's
backwards LZ, rasterised from

  - Noto Sans (SIL Open Font License 1.1) for Latin, Greek, Cyrillic and
    general symbols, and
  - WenQuanYi Micro Hei (GPL-3+ with the font exception, or Apache-2.0) for
    kana, kanji (JIS X 0208) and full-width forms.

The characters are the ones those scripts need, not a copy of Nintendo's
table; Nintendo's private-use symbols (U+E000..) are not included. The
table's RSA signature cannot be made, so the first 0x80 bytes hold a plain
marker instead; the emulator recognises it when a title checks the signature
through the DSi BIOS (SWI 22h) and answers with this file's header digest.
Every SHA-1 in the file is real. Needs Pillow and fontTools.
"""
import hashlib, struct, sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont, TTCollection

SIGNATURE_MARKER = b'DSperate TWLFontTable.dat: generated, not signed by Nintendo'.ljust(0x80, b'\0')
DATE = bytes([0x00, 0x13, 0x09, 0x26])     # the layout's date field, BCD, as in 2026-09-13

# name, cell width, cell height, baseline row (the first row below the text),
# the widest advance, and the two faces' pixel sizes. Cell geometry follows
# what DSi titles are written against; the pixel sizes put a capital 13/10/8
# pixels tall and a kanji 14/11/10.
SIZES = [
    ('TBF1_l.NFTR', 16, 21, 17, 17, 18, 15),
    ('TBF1_m.NFTR', 12, 16, 13, 13, 14, 12),
    ('TBF1_s.NFTR', 10, 12, 10, 11, 11, 10),
]


def coverage():
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180))
    cps |= set(range(0x384, 0x3CF)) | set(range(0x400, 0x460))            # Greek, Cyrillic
    cps |= {0x192, 0x2C6, 0x2C7, 0x2D8, 0x2D9, 0x2DA, 0x2DB, 0x2DC, 0x2DD}
    cps |= set(range(0x2010, 0x2027)) | {0x2030, 0x2032, 0x2033, 0x2039, 0x203A, 0x203B, 0x2044, 0x20AC}
    cps |= {0x2103, 0x2116, 0x2122} | set(range(0x2160, 0x216C)) | set(range(0x2190, 0x219A)) | {0x21D2, 0x21D4}
    cps |= set(range(0x3000, 0x3040)) | set(range(0x3041, 0x3097)) | set(range(0x309B, 0x30A0)) | set(range(0x30A1, 0x3100))
    cps |= set(range(0xFF01, 0xFF5F)) | set(range(0xFF61, 0xFFA0)) | set(range(0xFFE0, 0xFFE7))
    # JIS X 0208: every double-byte Shift-JIS code (symbols, kana, both kanji levels).
    for lead in list(range(0x81, 0xA0)) + list(range(0xE0, 0xF0)):
        for trail in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            try:
                ch = bytes([lead, trail]).decode('shift_jis')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                cps.add(ord(ch))
    return sorted(c for c in cps if not 0xE000 <= c <= 0xF8FF)


def cjk(c):
    return 0x2E80 <= c <= 0x9FFF or 0xF900 <= c <= 0xFAFF or 0xFF00 <= c <= 0xFFEF


def cmap_of(path):
    if path.lower().endswith('.ttc'):
        return TTCollection(path).fonts[0].getBestCmap()
    return TTFont(path).getBestCmap()


def render(face, c, w, h, base, shift, fullwidth):
    """The glyph as (bitmap rows of 0..3, left bearing, ink width, advance)."""
    img = Image.new('L', (w * 3, h), 0)
    ImageDraw.Draw(img).text((w, base + shift), chr(c), font=face, fill=255, anchor='ls')
    px = img.load()
    cols = [x for x in range(img.width) if any(px[x, y] for y in range(h))]
    advance = w if fullwidth else max(1, int(round(face.getlength(chr(c)))))
    if not cols:
        return [[0] * w for _ in range(h)], 0, 0, min(advance, 255)
    left, right = cols[0], min(cols[-1], cols[0] + w - 1)
    # Four levels, with a gamma lift so antialiased stems stay as dark as the
    # console's own font draws them at these sizes.
    level = [min(3, int(((v / 255) ** 0.7) * 3 + 0.5)) for v in range(256)]
    rows = [[level[px[x, y]] if x <= right and x < img.width else 0 for x in range(left, left + w)] for y in range(h)]
    return rows, left - w, right - left + 1, min(advance, 255)


def pad4(b):
    return b + b'\0' * (-len(b) % 4)


def build_nftr(codes, faces, w, h, base, maxw):
    tile_bytes = (w * h * 2 + 7) // 8
    glyphs, widths = bytearray(), bytearray()
    for c in codes:
        face, shift, full = faces(c)
        rows, left, ink, adv = render(face, c, w, h, base, shift, full)
        bits = 0
        for y in range(h):
            for x in range(w):
                bits = (bits << 2) | rows[y][x]
        glyphs += (bits << (tile_bytes * 8 - w * h * 2)).to_bytes(tile_bytes, 'big')
        widths += struct.pack('<bBB', max(-128, min(127, left)), ink, adv)

    # Character maps: a type-0 run for each block of 12 or more consecutive
    # code points (their tiles are consecutive too), one sorted type-2 map for
    # the rest.
    runs, singles, i = [], [], 0
    while i < len(codes):
        j = i
        while j + 1 < len(codes) and codes[j + 1] == codes[j] + 1:
            j += 1
        if j - i + 1 >= 12:
            runs.append((codes[i], codes[j], i))
        else:
            singles += [(codes[k], k) for k in range(i, j + 1)]
        i = j + 1

    cmap_bodies = [struct.pack('<HHI', first, last, 0) + pad4(struct.pack('<H', tile)) for first, last, tile in runs]
    cmap_bodies.append(struct.pack('<HHI', 0x0000, 0xFFFF, 2) +
                       pad4(struct.pack('<H', len(singles)) + b''.join(struct.pack('<HH', c, t) for c, t in singles)))

    finf_at = 0x10
    cglp_at = finf_at + 0x20
    cglp = pad4(struct.pack('<BBHBBBB', w, h, tile_bytes, base, maxw, 2, 0) + bytes(glyphs))
    cwdh_at = cglp_at + 8 + len(cglp)
    cwdh = pad4(struct.pack('<HHI', 0, len(codes) - 1, 0) + bytes(widths))
    cmap_at = cwdh_at + 8 + len(cwdh)

    out = bytearray()
    finf = struct.pack('<BBHBBBB', 0, h, 0, 0, w, w, 1) + struct.pack('<III', cglp_at + 8, cwdh_at + 8, cmap_at + 8) + bytes([h, maxw, base, 0])
    out += b'FNIF' + struct.pack('<I', 8 + len(finf)) + finf
    out += b'PLGC' + struct.pack('<I', 8 + len(cglp)) + cglp
    out += b'HDWC' + struct.pack('<I', 8 + len(cwdh)) + cwdh
    at = cmap_at
    for k, body in enumerate(cmap_bodies):
        size = 8 + 4 + len(body)
        nxt = at + size + 8 if k + 1 < len(cmap_bodies) else 0
        out += b'PAMC' + struct.pack('<I', size) + body[:8] + struct.pack('<I', nxt) + body[8:]
        at += size
    total = 0x10 + len(out)
    return struct.pack('<4sHHIHH', b'RTFN', 0xFEFF, 0x0102, total, 0x10, 3 + len(cmap_bodies)) + bytes(out)


# ---- the DSi's backwards LZ ------------------------------------------------------

def blz_decode(data):
    n = len(data)
    inc = struct.unpack_from('<I', data, n - 4)[0]
    if inc == 0:
        return bytes(data)
    hdr = data[n - 5]
    enc = data[n - 8] | data[n - 7] << 8 | data[n - 6] << 16
    raw = bytearray(n + inc)
    raw[:n - enc] = data[:n - enc]
    pak, pak_end, r, r_end = n - hdr, n - enc, len(raw), n - enc
    mask = flags = 0
    while r > r_end:
        mask >>= 1
        if not mask:
            pak -= 1; flags = data[pak]; mask = 0x80
        if not flags & mask:
            pak -= 1; r -= 1; raw[r] = data[pak]
        else:
            pak -= 2; pos = data[pak + 1] << 8 | data[pak]
            ln = min((pos >> 12) + 3, r - r_end)
            pos = (pos & 0xFFF) + 3
            for _ in range(ln):
                r -= 1; raw[r] = raw[r + pos]
    return bytes(raw)


def blz_encode(raw):
    """Compress `raw` so it decompresses in place: LZ77 over the reversed
    data, reversed back, with an uncompressed head long enough that the
    decoder's output never overtakes its unread input."""
    rev = raw[::-1]
    n = len(rev)
    heads = {}
    stream = bytearray()      # the forward token stream over `rev`
    marks = [(0, 0)]          # (input consumed, output written) at each token end
    i = 0
    flag_at, bit = -1, 0
    while i < n:
        if bit == 0:
            flag_at = len(stream); stream.append(0); bit = 0x80
        best_len, best_disp = 0, 0
        if i + 3 <= n:
            key = rev[i:i + 3]
            for p in reversed(heads.get(key, ())):
                disp = i - p
                if disp > 0x1002:
                    break
                if disp < 3:
                    continue
                ln = 3
                while ln < 18 and i + ln < n and rev[p + ln] == rev[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, disp
                    if ln == 18:
                        break
        step = best_len if best_len >= 3 else 1
        if best_len >= 3:
            stream[flag_at] |= bit
            v = (best_len - 3) << 12 | (best_disp - 3)
            stream += bytes([v >> 8, v & 0xFF])
        else:
            stream.append(rev[i])
        for k in range(i, i + step):
            if k + 3 <= n:
                lst = heads.setdefault(rev[k:k + 3], [])
                lst.append(k)
                if len(lst) > 16:
                    del lst[0]
        i += step
        bit >>= 1
        marks.append((i, len(stream)))
    # Cut at the token where consumed-minus-written reaches its running
    # maximum: everything before it (in raw order, the head) stays plain.
    best_k, best_s = 0, -1
    for k, (inp, outp) in enumerate(marks):
        s = inp - outp
        if s >= best_s:
            best_s, best_k = s, k
    inp, outp = marks[best_k]
    comp = bytes(stream[:outp])[::-1]
    # A flag byte whose group was cut short is fine: the decoder stops at the
    # head before reading the missing tokens.
    head = raw[:n - inp]
    body = head + comp
    footer_len = 8 + (-len(body) % 4)
    enc_len = len(comp) + footer_len
    total = len(body) + footer_len
    out = body + b'\xFF' * (footer_len - 8) + struct.pack('<I', enc_len)[:3] + bytes([footer_len]) + struct.pack('<I', len(raw) - total)
    assert len(raw) > total, 'incompressible'
    return out


def check_in_place(packed, raw_len):
    """Decode in one buffer the way an in-place decoder would; the output
    must never overwrite compressed bytes not yet read."""
    n = len(packed)
    inc = struct.unpack_from('<I', packed, n - 4)[0]
    if inc == 0:
        return True
    hdr = packed[n - 5]
    enc = packed[n - 8] | packed[n - 7] << 8 | packed[n - 6] << 16
    pak, pak_end, r = n - hdr, n - enc, n + inc
    mask = flags = 0
    while r > pak_end:
        mask >>= 1
        if not mask:
            pak -= 1; flags = packed[pak]; mask = 0x80
        if not flags & mask:
            pak -= 1; r -= 1
        else:
            pak -= 2; ln = min((((packed[pak + 1] << 8) | packed[pak]) >> 12) + 3, r - pak_end); r -= ln
        if r < pak:
            return False
    return True


def main(noto_path, cjk_path, out_path):
    noto_cmap, cjk_cmap = cmap_of(noto_path), cmap_of(cjk_path)
    codes = [c for c in coverage() if c in noto_cmap or c in cjk_cmap]
    entries, blobs = [], []
    offset = 0xA0 + 0x40 * len(SIZES)
    for name, w, h, base, maxw, latin_px, cjk_px in SIZES:
        noto = ImageFont.truetype(noto_path, latin_px)
        wqy = ImageFont.truetype(cjk_path, cjk_px, index=0)
        # Put a kanji's ink bottom on the baseline row, as the Latin capitals' is.
        probe = Image.new('L', (w * 3, h * 2), 0)
        ImageDraw.Draw(probe).text((w, h), '漢', font=wqy, fill=255, anchor='ls')
        bottom = max(y for y in range(h * 2) if any(probe.getpixel((x, y)) for x in range(w * 3)))
        cjk_shift = h - bottom                 # its ink bottom on the baseline row itself
        def faces(c):
            use_cjk = (cjk(c) and c in cjk_cmap) or c not in noto_cmap
            return (wqy, cjk_shift, cjk(c) and not 0xFF61 <= c <= 0xFFDC) if use_cjk else (noto, 0, False)
        nftr = build_nftr(codes, faces, w, h, base, maxw)
        packed = blz_encode(nftr)
        assert blz_decode(packed) == nftr, name + ': the compressor does not round-trip'
        assert check_in_place(packed, len(nftr)), name + ': not safe to decompress in place'
        entries.append(name.encode().ljust(0x20, b'\0') + struct.pack('<III', len(packed), offset, len(nftr)) + hashlib.sha1(packed).digest())
        blobs.append(packed + b'\0' * (-len(packed) % 16))
        print(f'{name}: {len(codes)} characters, {len(nftr)} bytes, {len(packed)} compressed', file=sys.stderr)
        offset += len(blobs[-1])
    table = b''.join(entries)
    header = DATE + bytes([len(SIZES), 0, 0, 0, 0, 0, 0, 0]) + hashlib.sha1(table).digest()
    assert len(header) == 0x20
    data = SIGNATURE_MARKER + header + table + b''.join(blobs)
    open(out_path, 'wb').write(data)
    print(f'{out_path}: {len(data)} bytes; header SHA-1 {hashlib.sha1(header).hexdigest()}', file=sys.stderr)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
