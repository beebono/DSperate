#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
"""Wrap a raw ARM9 and ARM7 binary in a DS cartridge header, for a direct boot
(dsperate-headless --direct): no banner, no secure area, no logo.

  mkrom.py CODE arm9.bin arm7.bin out.nds     CODE: the four-character game code
"""
import struct, sys

def crc16(data, v=0xFFFF):
    for b in data:
        v ^= b
        for _ in range(8):
            v = (v >> 1) ^ (0xA001 if v & 1 else 0)
    return v & 0xFFFF

code, a9p, a7p, outp = sys.argv[1:5]
a9, a7 = open(a9p, "rb").read(), open(a7p, "rb").read()
pad = lambda b: b + b"\0" * (-len(b) % 4)
a9, a7 = pad(a9), pad(a7)
A9_OFF = 0x8000                       # above the secure area, so nothing is decrypted
A7_OFF = (A9_OFF + len(a9) + 0x1FF) & ~0x1FF
size = 0x20000
while size < A7_OFF + len(a7): size *= 2
rom = bytearray(size)
h = rom
h[0x00:0x0C] = b"WIRETEST\0\0\0\0"
h[0x0C:0x10] = code.encode("ascii")[:4].ljust(4, b"#")
h[0x10:0x12] = b"00"
h[0x14] = max(0, size.bit_length() - 18)   # capacity: 128 KB << n
struct.pack_into("<4I", h, 0x20, A9_OFF, 0x02000000, 0x02000000, len(a9))
struct.pack_into("<4I", h, 0x30, A7_OFF, 0x02380000, 0x02380000, len(a7))
struct.pack_into("<I", h, 0x60, 0x00586000)   # ROMCTRL for normal commands
struct.pack_into("<I", h, 0x64, 0x001808F8)
struct.pack_into("<I", h, 0x80, size)
struct.pack_into("<I", h, 0x84, 0x4000)       # header size
struct.pack_into("<H", h, 0x15C, 0xCF56)      # the logo CRC field (the logo itself stays empty)
struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))
rom[A9_OFF:A9_OFF + len(a9)] = a9
rom[A7_OFF:A7_OFF + len(a7)] = a7
open(outp, "wb").write(rom)
print(f"{outp}: {size} bytes, ARM9 {len(a9)} at {A9_OFF:#x}, ARM7 {len(a7)} at {A7_OFF:#x}")
