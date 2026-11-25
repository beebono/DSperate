#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sort a sweep's games into categories from their --sweep-log files.

  classify.py OUT            (OUT as given to run_sweep.py)

Writes OUT/classify.tsv (one row per game, both phases side by side),
OUT/summary.md (counts, outliers) and OUT/sheet-<phase>-<category>.html
(the screenshot strips, for looking through by eye).

Per phase, a game is one of:
  failed        the run did not finish (crash, timeout, early exit)
  no-boot       never shows a non-uniform picture
  faulting      the CPUs raise undefined/abort exceptions (rarely legitimate)
  frozen        the picture stops changing and the sound stops, 30 s before the end
  static        the picture stops changing but sound plays (a still title, a menu)
  loop          the last 30 s cycle through a few pictures (blinking PRESS START)
  animated-2d   the last 30 s keep making new pictures, no 3D
  animated-3d   the same with 3D polygons (attract modes are usually here)
The mash phase is also compared with idle: regressed (worse category than idle:
the input broke it), progressed (the game took the input: at least ACCEPTED_MIN
presses or taps changed the picture within RESPONSE frames, where the picture had
been still before the press or the new one never appeared in the idle run), or
same (it did not: input ignored, or it needs a button or a touch spot the mash
does not use). The accepted count is in classify.tsv.
"""
import html, json, os, sys, collections

TAIL = 1800          # frames: the window the end state is judged on (30 s)
# The mash's input schedule (run_sweep.write_mash): A at START + k*PERIOD, Start 12
# frames later, a tap 24 frames later.
MASH_START, MASH_PERIOD, MASH_OFFSETS = 120, 40, (0, 12, 24)
RESPONSE = 10        # frames after an input in which a change counts as its answer
ACCEPTED_MIN = 3     # inputs answered for the game to count as having taken input
ORDER = ["failed", "no-boot", "faulting", "frozen", "static", "loop", "animated-2d", "animated-3d"]

def load(path):
    rows = []
    try:
        with open(path) as fh:
            next(fh)
            for l in fh:
                c = l.rstrip("\n").split("\t")
                if len(c) >= 17:
                    rows.append(c)
    except (OSError, StopIteration):
        pass
    return rows

def features(rows, frames):
    if not rows:
        return None
    pairs = [(r[1], r[2]) for r in rows]
    n = len(rows)
    tail = rows[-TAIL:] if n > TAIL else rows
    tp = [(r[1], r[2]) for r in tail]
    last_change = max((i for i in range(1, n) if pairs[i] != pairs[i - 1]), default=0)
    nonuniform = sum(1 for r in rows if r[3] == "-" or r[4] == "-")
    return dict(
        frames_run=n, finished=n >= frames,
        distinct=len(set(pairs)), tail_distinct=len(set(tp)),
        tail_changes=sum(1 for i in range(1, len(tp)) if tp[i] != tp[i - 1]),
        last_change=last_change, nonuniform=nonuniform,
        first_picture=next((int(r[0]) for r in rows if r[3] == "-" or r[4] == "-"), -1),
        polys_tail=sum(int(r[5]) for r in tail), polys=sum(int(r[5]) for r in rows),
        frames_3d=sum(1 for r in rows if int(r[5]) > 0),
        w2d=sum(int(r[6]) + int(r[7]) for r in rows),
        audio_tail=max((int(r[12]) for r in tail), default=0),
        faults=sum(int(r[13]) + int(r[14]) for r in rows),
        pcs_tail=len({(r[15], r[16]) for r in tail}),
        hashes=set(pairs), pairs=pairs,
    )

def accepted_inputs(fm, fi):
    """How many of the mash's inputs the picture answered (see the docstring)."""
    pairs, idle, n = fm["pairs"], fi["hashes"], 0
    for c in range(MASH_START, len(pairs), MASH_PERIOD):
        for o in MASH_OFFSETS:
            t = c + o
            if t < 4 or t >= len(pairs):
                continue
            pre = pairs[t - 1]
            still = all(pairs[k] == pre for k in range(t - 4, t))
            if any(pairs[k] != pre and (still or pairs[k] not in idle) for k in range(t, min(t + RESPONSE, len(pairs)))):
                n += 1
    return n

def category(f, rec):
    if f is None or rec.get("rc", 0) != 0 or not f["finished"]:
        return "failed"
    if f["nonuniform"] == 0:
        return "no-boot"
    if f["faults"] > 0:
        return "faulting"
    if f["tail_changes"] == 0:
        return "static" if f["audio_tail"] > 64 else "frozen"
    if f["tail_distinct"] <= 8:
        return "loop"
    return "animated-3d" if f["polys_tail"] > 0 else "animated-2d"

def results(out, phase):
    p, d = os.path.join(out, f"results-{phase}.jsonl"), {}
    if os.path.exists(p):
        for l in open(p):
            if l.strip():
                r = json.loads(l); d[r["file"]] = r
    return d

def main():
    out = sys.argv[1]
    res = {ph: results(out, ph) for ph in ("idle", "mash", "jit")}
    games = sorted(set(res["idle"]) | set(res["mash"]))
    rows, cats = [], {"idle": collections.defaultdict(list), "mash": collections.defaultdict(list)}
    outcome = collections.Counter()
    for g in games:
        row = {"file": g}
        feats = {}
        for ph in ("idle", "mash"):
            rec = res[ph].get(g)
            if rec is None:
                row[ph] = ""; continue
            f = features(load(os.path.join(out, "logs", ph, rec["id"] + ".tsv")), rec.get("frames", 0))
            feats[ph] = f
            c = category(f, rec); row[ph] = c; row["id"] = rec["id"]
            cats[ph][c].append((g, rec["id"], f))
            if f:
                row[ph + "_detail"] = (f"pic@{f['first_picture']} distinct={f['distinct']} tail={f['tail_distinct']} "
                                       f"3dframes={f['frames_3d']} w2d={f['w2d']} faults={f['faults']} lastchg={f['last_change']}")
        if row.get("idle") and row.get("mash"):
            fi, fm = feats.get("idle"), feats.get("mash")
            acc = accepted_inputs(fm, fi) if fi and fm else 0
            row["accepted"] = acc
            if ORDER.index(row["mash"]) < ORDER.index(row["idle"]) and row["mash"] in ORDER[:4]:
                o = "regressed"
            elif acc >= ACCEPTED_MIN:
                o = "progressed"
            else:
                o = "same"
            row["mash_vs_idle"] = o; outcome[o] += 1
        for f in feats.values():   # the per-frame data is done with: ~10 GB over a full DS set if kept
            if f:
                f.pop("pairs", None); f.pop("hashes", None)
        j = res["jit"].get(g)
        row["jit"] = j.get("verdict", "") + (f"@{j['first_diff']}" if j and j.get("verdict") == "DIFF" else "") if j else ""
        rows.append(row)
    cols = ["file", "idle", "mash", "mash_vs_idle", "accepted", "jit", "idle_detail", "mash_detail"]
    with open(os.path.join(out, "classify.tsv"), "w") as fh:
        fh.write("\t".join(cols) + "\n")
        for r in rows:
            fh.write("\t".join(str(r.get(c, "")) for c in cols) + "\n")
    # Outliers: anything that fails or dies in the mash, a mash that broke or
    # changed nothing, every JIT difference, and an idle run that died unless the
    # mash got past it (a still, silent title screen reads as frozen when idle).
    outl = [r for r in rows if r.get("mash") in ORDER[:4] or r.get("mash_vs_idle") in ("regressed", "same")
            or (r.get("idle") in ORDER[:4] and r.get("mash_vs_idle") != "progressed")
            or r.get("jit", "").startswith("DIFF")]
    with open(os.path.join(out, "summary.md"), "w") as fh:
        fh.write(f"# Compatibility sweep\n\n{len(games)} games classified.\n\n| category | idle | mash |\n|---|---|---|\n")
        for c in ORDER:
            fh.write(f"| {c} | {len(cats['idle'].get(c, []))} | {len(cats['mash'].get(c, []))} |\n")
        if outcome:
            fh.write("\nMash against idle: " + ", ".join(f"{k} {v}" for k, v in outcome.most_common()) + "\n")
        jv = collections.Counter(r["jit"].split("@")[0] for r in rows if r.get("jit"))
        if jv:
            fh.write("\nJIT check: " + ", ".join(f"{k} {v}" for k, v in jv.most_common()) + "\n")
        fh.write(f"\n## Outliers ({len(outl)})\n\n| game | idle | mash | mash vs idle | jit |\n|---|---|---|---|---|\n")
        for r in outl:
            fh.write(f"| {r['file']} | {r.get('idle', '')} | {r.get('mash', '')} | {r.get('mash_vs_idle', '')} | {r.get('jit', '')} |\n")
    for ph in ("idle", "mash"):
        for c, items in cats[ph].items():
            with open(os.path.join(out, f"sheet-{ph}-{c}.html"), "w") as fh:
                fh.write(f"<!DOCTYPE html><meta charset=utf-8><title>{ph}: {c}</title><style>body{{background:#222;color:#ddd;"
                         "font:13px sans-serif}}div{{margin:8px 0}}img{{image-rendering:pixelated;height:192px}}</style>"
                         f"<h1>{ph}: {c} ({len(items)})</h1>\n")
                for g, gid, f in sorted(items):
                    fh.write(f"<div><b>{html.escape(g)}</b><br><img loading=lazy src='shots/{ph}/{html.escape(gid)}.png'></div>\n")
    print(open(os.path.join(out, "summary.md")).read())

if __name__ == "__main__":
    main()
