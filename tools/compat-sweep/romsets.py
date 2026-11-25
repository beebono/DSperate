# SPDX-License-Identifier: GPL-3.0-or-later
"""What to run from one zip of a No-Intro set (shared by tabulate.py and run_sweep.py).

Two layouts:
  cartridge  one image inside: .nds (DS), .dsi (DSi-exclusive cart), .ids (iQue), .srl
  CDN        DSiWare as the update CDN serves it, decrypted: content files named by
             their 8-hex-digit content ID, one tmd.<version> per version, titleKey.bin,
             titlePass.txt. The app is content index 0 of the newest version's TMD.
"""
import os, struct

ROM_EXTS = (".nds", ".dsi", ".ids", ".srl")

def parse_tmd(data):
    """Title ID, title version and content records (id, index, type, size) of a TMD
    with an RSA-2048 signature (the only kind the DSi uses)."""
    if len(data) < 0x1E4:
        raise ValueError("TMD too short")
    tid = struct.unpack(">Q", data[0x18C:0x194])[0]
    ver, count = struct.unpack(">HH", data[0x1DC:0x1E0])
    recs = []
    for i in range(count):
        o = 0x1E4 + i * 36
        if o + 16 > len(data):
            raise ValueError("TMD content records cut short")
        recs.append(struct.unpack(">IHHQ", data[o:o + 16]))
    return tid, ver, recs

def pick(z):
    """For an open ZipFile: dict(kind, entry, size, crc, ext[, title_id, version, versions])
    or raise LookupError with the reason."""
    infos = {i.filename: i for i in z.infolist() if not i.is_dir()}
    tmds = sorted((n for n in infos if os.path.basename(n).startswith("tmd.")),
                  key=lambda n: int(n.rsplit(".", 1)[1]) if n.rsplit(".", 1)[1].isdigit() else -1)
    if tmds:
        tid, ver, recs = parse_tmd(z.read(tmds[-1]))
        app = next((r for r in recs if r[1] == 0), None)
        if app is None:
            raise LookupError("TMD has no content 0")
        name = next((n for n in infos if os.path.basename(n).lower() == f"{app[0]:08x}"), None)
        if name is None:
            raise LookupError(f"content {app[0]:08x} of {os.path.basename(tmds[-1])} missing")
        i = infos[name]
        return dict(kind="dsiware", entry=name, size=i.file_size, crc=f"{i.CRC:08x}", ext=".nds",
                    title_id=f"{tid:016x}", version=ver, versions=len(tmds))
    roms = [i for n, i in infos.items() if n.lower().endswith(ROM_EXTS)]
    if not roms:
        raise LookupError("no ROM image")
    i = roms[0]
    ext = os.path.splitext(i.filename)[1].lower()
    return dict(kind="dsi-cart" if ext == ".dsi" else "ds-cart", entry=i.filename, size=i.file_size,
                crc=f"{i.CRC:08x}", ext=ext, extra_entries=len(infos) - 1)
