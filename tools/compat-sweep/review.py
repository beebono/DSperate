#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A Markdown page of the games worth looking at by eye, with their screenshots.

  review.py SWEEP_DIR [SET ...]      (SWEEP_DIR holds one run_sweep.py output per set,
                                     e.g. ds/ dsiware/ dsi/; default: every subdirectory
                                     with a classify.tsv)

Writes SWEEP_DIR/REVIEW.md, linking the idle and mash screenshot strips by relative
path. Groups, most telling first, each game in the first group it fits:
  broken            failed, exited, no-boot or faulting in either phase
  regressed         the mash left it worse than idle
  stuck             frozen under the mash and took (almost) none of its input
  ignored input     took none of the mash's input, whatever the picture did
  frozen after input  took input, then ended frozen
  no progress       the rest of the mash-against-idle "same"
Small groups are open, large ones folded (<details>).
"""
import csv, os, sys, urllib.parse

BROKEN = ("failed", "exited", "no-boot", "faulting")
OPEN_MAX = 60   # groups up to this size are shown open
SHOTS = "frames 300, 1200, 2400, 3600, 5399 (5, 20, 40, 60, 90 s)"

def groups(rows):
    acc = lambda r: int(r.get("accepted") or 0)
    rules = [
        ("Broken", "failed, exited, no-boot or faulting in a phase",
         lambda r: r["idle"] in BROKEN or r["mash"] in BROKEN),
        ("Regressed", "the mash left the game worse off than idling",
         lambda r: r["mash_vs_idle"] == "regressed"),
        ("Stuck", "frozen at the end of the mash, and took fewer than 3 of its inputs",
         lambda r: r["mash"] == "frozen" and acc(r) < 3),
        ("Ignored input", "answered none of the mash's presses or taps",
         lambda r: r["mash_vs_idle"] and acc(r) == 0),
        ("Frozen after input", "took input, then ended frozen (could be a silent menu or a hang)",
         lambda r: r["mash"] == "frozen"),
        ("No progress", "the rest of the mash's 'same' (fewer than 3 inputs answered)",
         lambda r: r["mash_vs_idle"] == "same"),
    ]
    taken, out = set(), []
    for name, why, f in rules:
        g = [r for r in rows if r["file"] not in taken and f(r)]
        taken.update(r["file"] for r in g)
        out.append((name, why, g))
    return out

def link(path):
    return urllib.parse.quote(path)

def main():
    root = os.path.abspath(sys.argv[1])
    sets = sys.argv[2:] or sorted(d for d in os.listdir(root) if os.path.exists(os.path.join(root, d, "classify.tsv")))
    lines = ["# Sweep review: games worth a look", "",
             f"Each game: its idle run's strip, then its mash run's strip ({SHOTS}). "
             "Categories are the end state over the last 30 s; `accepted` is how many of the mash's inputs changed the picture.", ""]
    body, toc = [], []
    for s in sets:
        rows = list(csv.DictReader(open(os.path.join(root, s, "classify.tsv")), delimiter="\t"))
        ids = {}
        for ph in ("idle", "mash"):
            import json
            p = os.path.join(root, s, f"results-{ph}.jsonl")
            if os.path.exists(p):
                for l in open(p):
                    if l.strip():
                        r = json.loads(l); ids[r["file"]] = r["id"]
        body.append(f"## {s}"); body.append("")
        for name, why, g in groups(rows):
            if not g:
                continue
            anchor = f"{s}-{name}".lower().replace(" ", "-")
            toc.append(f"- [{s}: {name} ({len(g)})](#{anchor})")
            body.append(f'<a id="{anchor}"></a>'); body.append("")
            body.append(f"### {s}: {name} ({len(g)})"); body.append(""); body.append(f"_{why}._"); body.append("")
            fold = len(g) > OPEN_MAX
            if fold:
                body.append(f"<details><summary>show all {len(g)}</summary>"); body.append("")
            for r in sorted(g, key=lambda r: r["file"].lower()):
                gid = ids.get(r["file"])
                body.append(f"#### {r['file'][:-4] if r['file'].endswith('.zip') else r['file']}"); body.append("")
                body.append(f"idle **{r['idle'] or '-'}** · mash **{r['mash'] or '-'}** · {r['mash_vs_idle'] or '-'} · "
                            f"accepted {r.get('accepted') or 0}" + (f" · JIT {r['jit']}" if r.get('jit') and r['jit'] != 'same' else ""))
                body.append("")
                if gid:
                    for ph in ("idle", "mash"):
                        p = os.path.join(s, "shots", ph, gid + ".png")
                        if os.path.exists(os.path.join(root, p)):
                            body.append(f"{ph}: ![{ph}]({link(p)})"); body.append("")
            if fold:
                body.append("</details>"); body.append("")
    lines += ["## Contents", ""] + toc + [""] + body
    with open(os.path.join(root, "REVIEW.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(os.path.join(root, "REVIEW.md"), sum(1 for l in body if l.startswith("#### ")), "games")

if __name__ == "__main__":
    main()
