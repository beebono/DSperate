#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
"""Read a DSi NAND dump (with nocash footer) without melonDS.

    dsi_nand.py info    nand.bin
    dsi_nand.py ls      nand.bin [path]          # e.g. /title/00030004
    dsi_nand.py titles  nand.bin                 # DSiWare with .app/.sav sizes
    dsi_nand.py extract nand.bin /path/in/nand out-file
    dsi_nand.py map     nand.bin access.log         # trace_melonds --dsi log -> files touched
    dsi_nand.py bootblobs nand.bin out.bin           # the 0x154 bytes a DSi direct boot copies into main RAM

Key derivation and sector crypto follow melonDS DSi_NAND.cpp:42-112 (FAT
key from the ConsoleID, IV = SHA-1 of the eMMC CID, AES-CTR with byte-
reversed 16-byte blocks).  This is the reference FAT reader the phase-3
synthetic-NAND unit test checks against; it is independent of the C++.
Needs the `cryptography` package.
"""
import hashlib, struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

M128 = (1 << 128) - 1

def derive_normal_key(key_x: bytes, key_y: bytes) -> bytes:
    x = int.from_bytes(key_x, 'little'); y = int.from_bytes(key_y, 'little')
    n = ((x ^ y) + 0xFFFEFB4E295902582A680F5F1A4F3E79) & M128
    n = ((n << 42) | (n >> 86)) & M128
    return n.to_bytes(16, 'little')

def bswap_blocks(b: bytes) -> bytes:
    return b''.join(b[i:i + 16][::-1] for i in range(0, len(b), 16))

class Nand:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(-64, 2); ft = self.f.read(64)
        if ft[:16] != b'DSi eMMC CID/CPU':
            self.f.seek(0x000FF800); ft = self.f.read(64)
            if ft[:16] != b'DSi eMMC CID/CPU': sys.exit('no nocash footer')
        self.cid = ft[16:32]; self.console_id = struct.unpack('<Q', ft[32:40])[0]
        lo, hi = self.console_id & 0xFFFFFFFF, self.console_id >> 32
        kx = struct.pack('<4I', lo, lo ^ 0x24EE6906, hi ^ 0xE65B601D, hi)
        ky = struct.pack('<4I', 0x0AB9DC76, 0xBD4DC4D3, 0x202DDD1D, 0xE1A00005)
        self.key = derive_normal_key(kx, ky)[::-1]
        self.iv = int.from_bytes(hashlib.sha1(self.cid).digest()[:16][::-1], 'big')
        mbr = self.read(0, 512)
        assert mbr[0x1FE:0x200] == b'\x55\xAA', 'MBR did not decrypt: bad footer/keys'
        self.parts = [(mbr[0x1BE + i * 16 + 4], struct.unpack_from('<II', mbr, 0x1BE + i * 16 + 8)) for i in range(4)]
        self.base = self.parts[0][1][0] * 512
        b = self.read(self.base, 512)
        self.bps, self.spc, rsv, nf, self.rootent, _, _, spf = struct.unpack_from('<HBHBHHBH', b, 0x0B)
        self.fat_off = self.base + rsv * self.bps
        self.root_off = self.fat_off + nf * spf * self.bps
        self.data_off = self.root_off + self.rootent * 32
        self.fat = self.read(self.fat_off, spf * self.bps)

    def read(self, off, n):
        ctr = ((self.iv + (off >> 4)) & M128).to_bytes(16, 'big')
        dec = Cipher(algorithms.AES(self.key), modes.CTR(ctr)).decryptor()
        self.f.seek(off); return bswap_blocks(dec.update(bswap_blocks(self.f.read(n))))

    def chain(self, c):
        out = []
        while 2 <= c < 0xFFF8: out.append(c); c = struct.unpack_from('<H', self.fat, c * 2)[0]
        return out

    def file_data(self, cluster, size=None):
        if cluster == 0: return self.read(self.root_off, self.rootent * 32)
        cs = self.spc * self.bps
        d = b''.join(self.read(self.data_off + (c - 2) * cs, cs) for c in self.chain(cluster))
        return d if size is None else d[:size]

    def listdir(self, cluster):
        data = self.file_data(cluster); ents = []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0: break
            if e[0] == 0xE5 or e[11] == 0x0F: continue
            name = e[:8].decode('latin1').strip()
            if e[8:11].strip(): name += '.' + e[8:11].decode('latin1').strip()
            ents.append((name, e[11], struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]))
        return ents

    def walk(self):
        """Yield (path, cluster, size, attr) for every file and directory."""
        stack = [('', 0)]
        while stack:
            base, cl = stack.pop()
            for name, attr, c, size in self.listdir(cl):
                if name in ('.', '..'): continue
                path = base + '/' + name
                yield path, c, size, attr
                if attr & 0x10: stack.append((path, c))

    def owner_index(self):
        """Map absolute byte offset -> owner, at cluster granularity."""
        cs = self.spc * self.bps; idx = {}
        for path, c, size, attr in self.walk():
            for k, cl in enumerate(self.chain(c)):
                idx[self.data_off + (cl - 2) * cs] = (path + ('/' if attr & 0x10 else ''), k * cs)
        return idx

    def lookup(self, path):
        cl, attr, size = 0, 0x10, 0
        for part in [p for p in path.split('/') if p]:
            e = next((e for e in self.listdir(cl) if e[0].lower() == part.lower()), None)
            if e is None: sys.exit(f'not found: {path}')
            _, attr, cl, size = e
        return cl, attr, size

def main(argv):
    if len(argv) < 3: sys.exit(__doc__)
    cmd, n = argv[1], Nand(argv[2])
    if cmd == 'info':
        print(f'CID {n.cid.hex()}  ConsoleID {n.console_id:016x}')
        for t, (s, c) in n.parts: print(f'  part type {t:#04x} start {s * 512:#x} sectors {c}')
        print(f'  FAT16 bps={n.bps} spc={n.spc} data@{n.data_off:#x}')
    elif cmd == 'ls':
        cl, attr, _ = n.lookup(argv[3] if len(argv) > 3 else '/')
        for name, a, c, size in n.listdir(cl):
            print(f'{"d" if a & 0x10 else "-"} {size:10d} {name}')
    elif cmd == 'titles':
        cl, _, _ = n.lookup('/title/00030004')
        for name, a, c, _ in n.listdir(cl):
            if name in ('.', '..'): continue
            row = {}
            for dn, da, dc, _ in n.listdir(c):
                if dn.lower() in ('content', 'data'):
                    row[dn.lower()] = [(x[0], x[3]) for x in n.listdir(dc) if x[0] not in ('.', '..')]
            print(bytes.fromhex(name).decode(), name, row.get('content'), row.get('data'))
    elif cmd == 'extract':
        cl, attr, size = n.lookup(argv[3])
        open(argv[4], 'wb').write(n.file_data(cl, size)); print(f'{size} bytes -> {argv[4]}')
    elif cmd == 'bootblobs':
        # What melonDS DSi::SetupDirectBoot copies from the NAND: the newer
        # TWLCFG (byte 0x81 is the counter) bytes 0x88..0x1AF -> 0x02000400,
        # HWINFO_N 0x88..0x9B -> 0x02000600, HWINFO_S 0x88..0x9F -> 0x02FFFD68.
        # DSperate's --dsi-boot takes the concatenation (NDS::load_dsi_boot_blobs).
        cfgs = []
        for i in (0, 1):
            try:
                cl, _, size = n.lookup(f'/shared1/TWLCFG{i}.dat'); cfgs.append(n.file_data(cl, size))
            except Exception: cfgs.append(None)
        v = [c[0x81] if c else -1 for c in cfgs]
        cfg = cfgs[1] if v[1] > v[0] else cfgs[0]
        if cfg is None: sys.exit('no TWLCFG in this NAND')
        cl, _, size = n.lookup('/sys/HWINFO_N.dat'); hn = n.file_data(cl, size)
        cl, _, size = n.lookup('/sys/HWINFO_S.dat'); hs = n.file_data(cl, size)
        out = cfg[0x88:0x88 + 0x128] + hn[0x88:0x88 + 0x14] + hs[0x88:0x88 + 0x18]
        assert len(out) == 0x154
        open(argv[3], 'wb').write(out); print(f'TWLCFG{1 if v[1] > v[0] else 0} (counters {v}), {len(out)} bytes -> {argv[3]}')
    elif cmd == 'map':
        idx = n.owner_index(); cs = n.spc * n.bps
        from collections import Counter, defaultdict
        hits = defaultdict(lambda: Counter()); first = {}
        for i, line in enumerate(open(argv[3])):
            rw, off, ln = line.split(); off = int(off, 16)
            if off < n.base: who = '<before partition 0: stage2/boot>'
            elif off < n.fat_off: who = '<part0 boot sector>'
            elif off < n.root_off: who = '<part0 FAT>'
            elif off < n.data_off: who = '<part0 root dir>'
            elif off >= n.base + n.parts[0][1][1] * 512: who = '<part1/2: photo/other>'
            else:
                o = idx.get(off - (off - n.data_off) % cs); who = o[0] if o else '<part0 free cluster>'
            hits[who][rw] += 1; first.setdefault(who, i)
        for who in sorted(hits, key=lambda w: first[w]):
            print(f"{hits[who]['r']:8d} r {hits[who]['w']:6d} w  first@{first[who]:<8d} {who}")
    else: sys.exit(__doc__)

if __name__ == '__main__': main(sys.argv)
