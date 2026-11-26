#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Play through a checklist of the sweep's games by hand, one at a time, and record a verdict.

  guide.py --list MENUS.md --set NAME=SETDIR [--set ...] --out DIR [options]

For each unticked game of the list (review.py --menus writes one; its "## <set>"
headings say which set a game is in), the game is unpacked into DIR/roms, run in
the SDL frontend, and when its window is closed you answer:

  y  works (got past its menus)     the box is ticked in the list
  n  unsure (needs a translation, a peripheral, more play)
  x  clearly broken or hung
  r  run it again          s  skip for now          q  quit

Then an optional note. Verdicts go to DIR/guided.tsv (date, set, file, verdict,
note); a game already there is not asked again, so it resumes. Battery saves
and save states go to DIR/saves and DIR/states, never beside the set; the
unpacked ROM is deleted after its verdict (keep it with --keep-roms). The set is
only read: the frontend never sees a zip (its zip cache would write
<set>/.dsperate), and the script stops if that directory appears. Controls,
video and audio come from your usual config (--config to use another).
"""
import argparse, datetime, os, re, shutil, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from romsets import pick
from run_sweep import game_id, REPO

ITEM = re.compile(r"^- \[( |x)\] \*\*(.+?)\*\*")

def read_list(path):
    """[(set, title, line index, ticked)] from a review.py --menus page."""
    out, s = [], None
    for i, l in enumerate(open(path).read().split("\n")):
        h = re.match(r"^## (\S+)", l)
        if h:
            s = h.group(1)
        m = ITEM.match(l)
        if m and s:
            out.append((s, m.group(2), i, m.group(1) == "x"))
    return out

def tick(path, idx):
    lines = open(path).read().split("\n")
    lines[idx] = lines[idx].replace("- [ ]", "- [x]", 1)
    open(path + ".tmp", "w").write("\n".join(lines))
    os.replace(path + ".tmp", path)

def find_zip(set_dir, title):
    for cand in (title + ".zip", title):
        p = os.path.join(set_dir, cand)
        if os.path.isfile(p):
            return p
    for root, _, files in os.walk(set_dir):   # a set with subdirectories
        if ".dsperate" in root:
            continue
        for f in files:
            if f in (title + ".zip",):
                return os.path.join(root, f)
    return None

def unpack(src, romd):
    gid = game_id(src)
    with zipfile.ZipFile(src) as z:
        e = pick(z)
        dst = os.path.join(romd, gid + e["ext"])   # named per game: its save is named after it
        if not os.path.exists(dst):
            with z.open(e["entry"]) as fi, open(dst + ".part", "wb") as fo:
                shutil.copyfileobj(fi, fo, 4 << 20)
            os.replace(dst + ".part", dst)
    return dst, e["kind"]

def machine(kind, a):
    args = ["--bios9", os.path.join(a.bios, "bios9.bin"), "--bios7", os.path.join(a.bios, "bios7.bin")]
    if kind == "ds-cart":
        return args + ["--firmware", os.path.join(a.bios, "firmware.bin")]
    return args + ["--bios9i", os.path.join(a.dsi_bios, "dsi_bios9.bin"), "--bios7i", os.path.join(a.dsi_bios, "dsi_bios7.bin"),
                   "--firmware", os.path.join(a.dsi_bios, "dsi_firmware.bin"), "--dsi-offline"]

def ask(prompt, choices):
    while True:
        try:
            v = input(prompt).strip().lower()
        except EOFError:
            return "q"
        if v in choices:
            return v

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", required=True, help="a review.py --menus page (MENUS.md)")
    ap.add_argument("--set", action="append", default=[], help="NAME=DIR: the ROM set behind the list's '## NAME' heading")
    ap.add_argument("--out", required=True, help="where verdicts, saves, states and unpacked ROMs go")
    ap.add_argument("--bios", default=os.environ.get("DS_BIOS"))
    ap.add_argument("--dsi-bios", default=os.environ.get("DS_DSI_BIOS"))
    ap.add_argument("--dsperate", default=os.path.join(REPO, "build/host/src/frontend/sdl/dsperate"))
    ap.add_argument("--config", help="settings file for the frontend (default: your usual one)")
    ap.add_argument("--only", help="regex on the title")
    ap.add_argument("--keep-roms", action="store_true")
    ap.add_argument("--extra", default="", help="more frontend arguments, space-separated")
    a = ap.parse_args()
    if not a.bios:
        sys.exit("need --bios or $DS_BIOS")
    sets = dict(s.split("=", 1) for s in a.set)
    out = os.path.abspath(a.out)
    for s in sets.values():
        if out == os.path.abspath(s) or out.startswith(os.path.abspath(s) + os.sep):
            sys.exit("--out must not be inside a set")
    romd, saves, states = (os.path.join(out, d) for d in ("roms", "saves", "states"))
    for d in (romd, saves, states):
        os.makedirs(d, exist_ok=True)
    res = os.path.join(out, "guided.tsv")
    done = set()
    if os.path.exists(res):
        done = {tuple(l.rstrip("\n").split("\t")[1:3]) for l in open(res) if l.strip() and not l.startswith("date\t")}
    else:
        open(res, "w").write("date\tset\tfile\tverdict\tnote\n")
    items = [it for it in read_list(a.list) if not it[3] and (it[0], it[1]) not in done
             and (not a.only or re.search(a.only, it[1]))]
    missing = sorted({s for s, *_ in items} - set(sets))
    if missing:
        sys.exit(f"no --set for: {', '.join(missing)}")
    print(f"{len(items)} games to go ({len(done)} already answered). Close the game's window to answer.\n")
    for n, (s, title, idx, _) in enumerate(items, 1):
        src = find_zip(sets[s], title)
        if not src:
            print(f"[{n}/{len(items)}] {title}: not found in {sets[s]}, skipped"); continue
        try:
            rom, kind = unpack(src, romd)
        except (zipfile.BadZipFile, LookupError, ValueError, OSError) as e:
            print(f"[{n}/{len(items)}] {title}: cannot unpack ({e}), skipped"); continue
        if kind != "ds-cart" and not a.dsi_bios:
            print(f"[{n}/{len(items)}] {title}: {kind} needs --dsi-bios, skipped"); continue
        cmd = [a.dsperate, rom] + machine(kind, a) + ["--saves", saves, "--states", states] + a.extra.split()
        if a.config:
            cmd += ["--config", a.config]
        while True:
            print(f"[{n}/{len(items)}] {s}: {title}  ({kind})", flush=True)
            subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "last-run.log"), "w"))
            for sd in sets.values():
                if os.path.exists(os.path.join(sd, ".dsperate")):
                    sys.exit(f"STOPPED: something wrote {sd}/.dsperate; the set must stay untouched")
            v = ask("  y works / n unsure / x broken / r run again / s skip / q quit: ", "ynxrsq")
            if v != "r":
                break
        if v == "q":
            print("stopped; run again to carry on"); return
        if v != "s":
            note = input("  note (enter for none): ").strip().replace("\t", " ")
            with open(res, "a") as fh:
                fh.write(f"{datetime.datetime.now():%Y-%m-%d %H:%M}\t{s}\t{title}\t{ {'y': 'works', 'n': 'unsure', 'x': 'broken'}[v] }\t{note}\n")
            if v == "y":
                tick(a.list, idx)
        if not a.keep_roms and v != "s":
            os.remove(rom)
    print("all done")

if __name__ == "__main__":
    main()
