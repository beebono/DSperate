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

  review.py SWEEP_DIR --menus [--second DIR2]

Writes SWEEP_DIR/MENUS.md instead: a checklist of the games that took the mash's
input but may have got no further than their first menus (they took 3 to
MENU_ACC-1 inputs and ended on a still or looping screen, or kept animating with
fewer than MENU_DISTINCT distinct pictures). DIR2 is a second pass over some of
the games (run_sweep.py --mash wide, laid out like SWEEP_DIR); per game the run
that took the most input is the one shown.
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

MENU_ACC, MENU_DISTINCT = 20, 200

def detail(r, k):
    import re
    m = re.search(k + r"=(-?\d+)", r.get("mash_detail") or "")
    return int(m.group(1)) if m else 0

def maybe_menus(r):
    acc = int(r.get("accepted") or 0)
    if r["mash_vs_idle"] != "progressed" or acc >= MENU_ACC:
        return False
    return r["mash"] in ("static", "loop") or (r["mash"].startswith("animated") and detail(r, "distinct") < MENU_DISTINCT)

def menus(root, second):
    import json
    def load(d, s):
        p = os.path.join(d, s, "classify.tsv")
        if not os.path.exists(p):
            return {}
        rows = {r["file"]: dict(r, dir=d) for r in csv.DictReader(open(p), delimiter="\t")}
        for l in open(os.path.join(d, s, "results-mash.jsonl")):
            j = json.loads(l)
            if j["file"] in rows: rows[j["file"]]["gid"] = j["id"]
        return rows
    sets = sorted(d for d in os.listdir(root) if os.path.exists(os.path.join(root, d, "classify.tsv")))
    lines = ["# Possibly only the first menus", "",
             f"Games that took the mash's input (3 to {MENU_ACC - 1} inputs answered) but ended on a still or looping screen, "
             f"or kept animating with fewer than {MENU_DISTINCT} distinct pictures. Each shows the run with the most input taken "
             "(the standard mash, or the wide mash where it was rerun), frames 300, 1200, 2400, 3600, 5399. "
             "Tick a box (`[x]`) once a game is seen to be past its menus or fine.", ""]
    total = 0
    for s in sets:
        a, b = load(root, s), load(second, s) if second else {}
        pick = {}
        for f, r in a.items():
            r2 = b.get(f)
            best = r2 if r2 and int(r2.get("accepted") or 0) >= int(r.get("accepted") or 0) else r
            if maybe_menus(best):
                pick[f] = best
        if not pick:
            continue
        lines += [f"## {s} ({len(pick)})", ""]
        for f in sorted(pick, key=str.lower):
            r = pick[f]; total += 1
            which = "wide" if r["dir"] != root else "standard"
            lines.append(f"- [ ] **{f[:-4] if f.endswith('.zip') else f}**: {r['mash']}, accepted {r.get('accepted')}, "
                         f"{detail(r, 'distinct')} pictures ({which} mash)")
            png = os.path.join(r["dir"], s, "shots", "mash", r.get("gid", "") + ".png")
            if os.path.exists(png):
                lines += ["", f"  ![{s}]({link(os.path.relpath(png, root))})", ""]
    with open(os.path.join(root, "MENUS.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(os.path.join(root, "MENUS.md"), total, "games")

def main():
    if "--menus" in sys.argv:
        argv = [x for x in sys.argv[1:] if x != "--menus"]
        second = None
        if "--second" in argv:
            i = argv.index("--second"); second = os.path.abspath(argv[i + 1]); del argv[i:i + 2]
        return menus(os.path.abspath(argv[0]), second)
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
