#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tabulate a No-Intro DS, DSi or DSiWare (CDN) set (zips, any depth) into sweep lists.

  tabulate.py SET_DIR OUT_DIR

Read-only: it opens each zip's central directory (and, for DSiWare, reads the
TMDs to find the newest version's app; see romsets.py) and nothing else.
Writes into OUTDIR:
  games.tsv   one row per zip: name tags, kind (ds-cart, dsi-cart, dsiware), the
              entry to run, sizes, CRC, DSiWare title ID/version, status
  titles.tsv  one row per base title, with the preferred dump for the sweep
  summary.md  counts
"""
import os, re, sys, zipfile, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from romsets import pick

src, out = sys.argv[1], sys.argv[2]
REGION_PREF = ["USA", "World", "Europe", "United Kingdom", "Australia",
               "Japan", "Korea", "China", "Taiwan"]
FLAG_WORDS = ("Beta", "Proto", "Demo", "Kiosk", "Sample", "Preview",
              "Debug", "Promo", "Unl", "Pirate", "Aftermarket", "Program")
LANG = re.compile(r"^[A-Z][a-z](-[A-Z][a-z]+)?(,[A-Z][a-z](-[A-Z][a-z]+)?)*$")

def parse(stem):
    tags = re.findall(r"\(([^)]*)\)", stem)
    title = re.split(r"\s+\(", stem, 1)[0].strip()
    region = tags[0] if tags else ""
    langs = next((t for t in tags[1:] if LANG.match(t)), "")
    flags = [t for t in tags[1:] if t != langs]
    nonretail = [f for f in flags if f.split()[0] in FLAG_WORDS]
    dsi = ("DSi-exclusive" if any("NDSi Exclusive" in f or "DSi Exclusive" in f for f in flags)
           else "DSi-enhanced" if any("NDSi Enhanced" in f or "DSi Enhanced" in f for f in flags)
           else "")
    rev = next((f for f in flags if re.match(r"^(Rev|v)\s?[\d.]+", f)), "")
    return title, region, langs, flags, nonretail, dsi, rev

rows = []
for root, _, files in os.walk(src):
    for f in sorted(files):
        if not f.lower().endswith(".zip"):
            continue
        p = os.path.join(root, f)
        rel = os.path.relpath(p, src)
        stem = f[:-4]
        title, region, langs, flags, nonretail, dsi, rev = parse(stem)
        inner = usize = crc = kind = tid = ver = ""
        status = "ok"
        try:
            with zipfile.ZipFile(p) as z:
                e = pick(z)
                inner, usize, crc, kind = e["entry"], e["size"], e["crc"], e["kind"]
                tid, ver = e.get("title_id", ""), e.get("version", "")
                if e.get("extra_entries"):
                    status = f"multi({e['extra_entries'] + 1})"
        except LookupError as e:
            status = "no-rom: " + str(e)
        except (zipfile.BadZipFile, ValueError, OSError) as e:
            status = "bad-zip: " + str(e).replace("\t", " ")
        rows.append(dict(file=rel, title=title, region=region, langs=langs,
                         dsi=dsi, rev=rev, nonretail=",".join(nonretail),
                         flags="; ".join(flags), kind=kind, inner=inner,
                         title_id=tid, version=ver,
                         rom_bytes=usize, zip_bytes=os.path.getsize(p),
                         crc32=crc, status=status))

cols = ["file", "title", "region", "langs", "dsi", "rev", "nonretail", "flags", "kind",
        "inner", "title_id", "version", "rom_bytes", "zip_bytes", "crc32", "status"]
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "games.tsv"), "w") as fh:
    fh.write("\t".join(cols) + "\n")
    for r in rows:
        fh.write("\t".join(str(r[c]) for c in cols) + "\n")

def rank(r):
    reg = r["region"].split(", ")
    ri = min((REGION_PREF.index(x) for x in reg if x in REGION_PREF), default=len(REGION_PREF))
    variant = any(v in r["flags"] for v in ("Virtual Console", "Switch Online", "Alt"))
    return (not r["status"].startswith(("ok", "multi")), bool(r["nonretail"]), variant, ri,
            "En" not in r["langs"].split(",") and ri > 1,
            # newest revision first
            [-int(x) for x in re.findall(r"\d+", r["rev"])] or [0])

groups = collections.defaultdict(list)
for r in rows:
    if not r["title"].startswith("[BIOS]"):
        groups[r["title"].lower()].append(r)
with open(os.path.join(out, "titles.tsv"), "w") as fh:
    fh.write("title\tdumps\tretail_dumps\tdsi\tregions\tkind\tpick\tpick_status\n")
    for k in sorted(groups):
        g = sorted(groups[k], key=rank)
        best = g[0]
        regions = sorted({x for r in g for x in r["region"].split(", ") if x})
        dsi = next((r["dsi"] for r in g if r["dsi"]), "")
        fh.write(f"{best['title']}\t{len(g)}\t{sum(not r['nonretail'] for r in g)}\t"
                 f"{dsi}\t{', '.join(regions)}\t{best['kind']}\t{best['file']}\t{best['status']}\n")

st = collections.Counter(r["status"].split(":")[0] for r in rows)
retail_titles = sum(any(not r["nonretail"] for r in g) for g in groups.values())
with open(os.path.join(out, "summary.md"), "w") as fh:
    fh.write(f"# Set tabulation\n\nSource: `{src}`\n\n")
    fh.write(f"- zips: {len(rows)}\n- base titles: {len(groups)} "
             f"({retail_titles} with a retail dump)\n")
    fh.write(f"- DSi-enhanced dumps: {sum(r['dsi']=='DSi-enhanced' for r in rows)}, "
             f"DSi-exclusive: {sum(r['dsi']=='DSi-exclusive' for r in rows)}\n")
    kinds = collections.Counter(r["kind"] for r in rows if r["kind"])
    fh.write("- kinds: " + ", ".join(f"{k} {v}" for k, v in kinds.most_common()) + "\n")
    fh.write(f"- non-retail (beta/proto/demo/kiosk/...): {sum(bool(r['nonretail']) for r in rows)}\n")
    fh.write("- status: " + ", ".join(f"{k} {v}" for k, v in st.most_common()) + "\n")
    bad = [r for r in rows if r["status"] != "ok" and not r["status"].startswith("multi")]
    if bad:
        fh.write("\n## Problem zips\n\n")
        for r in bad:
            fh.write(f"- `{r['file']}`: {r['status']}\n")
print(open(os.path.join(out, "summary.md")).read())
