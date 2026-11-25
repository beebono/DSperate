#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where on the bottom screen does a tap get each stuck game past its screen?

  probe_touch.py --sweep DIR --set NAME=SETDIR [--set ...] --names FILE [options]
  probe_touch.py --sweep DIR --plan [--points K] [--min-change F]

FILE: text naming the games (one per line or several per line, as in a review
list): every game of the sweep whose file name (without .zip) occurs in it is
probed. For each one the mash run is replayed (deterministic: it ends on the
sweep's own last frame, checked against its sweep log) with a save state at
its end; from that state the bottom screen is tapped on a grid (--step px,
--hold frames) and the frame --wait frames later is compared with the no-tap
frame. The share of both screens' pixels that changed is the tap's effect: a
button that moves the game on changes most of the picture, a keyboard key or a
highlight a little. Results: DIR/probe/<set>/<id>.json and a heat map PNG
(the stuck screen, the bottom tinted: red >= 5% changed, magenta >= 1%, blue any).

--verify (after a probe, same arguments) takes each screen with more than one
high-change result (points grouped by their exact change: one button, one
picture), taps the most central point of each and checks whether it lands on
a screen the mash run had already shown (its hashes against every frame of the
sweep's mash log, the stuck screen itself excluded): a Back or No. --plan
leaves those out.

--plan reads the results and picks, greedily, the K tap points that move on
the most games (a game counts as moved by a point when the change there is at
least --min-change), for a mash that taps them in turn (tools/mkdsin.py).
"""
import argparse, concurrent.futures as cf, json, os, re, subprocess, sys, tempfile, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_sweep import extract, machine_args, REPO

FSZ = 256 * 192 * 4 * 2   # one --dump-frames frame: both screens, 0xAARRGGBB

def frame(path):
    with open(path, "rb") as fh:
        d = fh.read()
    return d[-FSZ:] if len(d) >= FSZ else None

def changed_fast(a, b):
    import array
    x, y = array.array("I", a), array.array("I", b)
    return sum(1 for p, q in zip(x, y) if (p ^ q) & 0xFFFFFF) / len(x)

def run(cmd):
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600).returncode

def heatmap(base, grid, step, png):
    from PIL import Image
    im = Image.frombuffer("RGBA", (256, 384), base, "raw", "BGRA", 0, 1).convert("RGB")
    ov = Image.new("RGB", (256, 384), (0, 0, 0)); mask = Image.new("L", (256, 384), 0)
    px, mk = ov.load(), mask.load()
    for (x, y), f in grid.items():
        c = (255, 0, 0) if f >= 0.05 else (255, 0, 255) if f >= 0.01 else (0, 90, 255) if f > 0 else None
        if c:
            for i in range(x - step // 2, x + step // 2):
                for j in range(y - step // 2, y + step // 2):
                    if 0 <= i < 256 and 0 <= j < 192:
                        px[i, 192 + j] = c; mk[i, 192 + j] = 170
    im.paste(ov, (0, 0), mask)
    im.save(png)

def state(g, a, tmp):
    """Replay the game's mash run with a save state at its end: (base command, state, rom, scratch, rerun matches)."""
    gid = g["id"]
    rom, scratch, kind = extract(os.path.join(a.sets[g["set"]], g["file"]), tmp, gid)
    base = [a.headless] + machine_args(kind, a) + ["--direct"]
    st, hf = os.path.join(tmp, gid + ".st"), os.path.join(tmp, gid + ".h")
    mash = os.path.join(a.sweep, g["set"], f"mash-{g['frames']}.dsin")
    run(base + ["--frames", str(g["frames"]), "--replay", mash, "--save-state-at", f"{g['frames']}:{st}", "--hash-frames", hf, rom])
    try:   # the rerun must end where the sweep's mash run did
        last = open(hf).read().split("\n")[-2].split()
        sl = open(os.path.join(a.sweep, g["set"], "logs", "mash", gid + ".tsv")).read().split("\n")[-2].split("\t")
        ok = last[1:3] == sl[1:3]
    except (OSError, IndexError):
        ok = None
    if os.path.exists(hf): os.remove(hf)
    return base, st, rom, scratch, ok

def cleanup(st, scratch):
    if os.path.exists(st): os.remove(st)
    if scratch:
        import shutil; shutil.rmtree(scratch, ignore_errors=True)

def probe(g, a, outd, tmp):
    gid = g["id"]
    res = os.path.join(outd, gid + ".json")
    if os.path.exists(res):
        return json.load(open(res))
    rec = {"file": g["file"], "id": gid, "set": g["set"], "step": a.step, "hold": a.hold, "wait": a.wait}
    base, st, rom, scratch, rec["rerun_matches"] = state(g, a, tmp)
    try:
        if not os.path.exists(st):
            rec["error"] = "no state"; return rec
        def shot(touch, name):
            raw = os.path.join(tmp, f"{gid}.{name}.raw")
            cmd = base + ["--load-state", st, "--frames", str(a.wait + 1), "--dump-frames", raw, "--dump-at", str(a.wait)]
            if touch:
                cmd += ["--touch", f"1:{touch[0]},{touch[1]}:{a.hold}"]
            run(cmd + [rom])
            try:
                return frame(raw)
            finally:
                if os.path.exists(raw): os.remove(raw)
        b = shot(None, "base")
        if b is None:
            rec["error"] = "no frame"; return rec
        grid = {}
        for y in range(a.step // 2, 192, a.step):
            for x in range(a.step // 2, 256, a.step):
                f = shot((x, y), f"{x}_{y}")
                grid[(x, y)] = round(changed_fast(b, f), 4) if f else -1
        rec["grid"] = [[x, y, f] for (x, y), f in grid.items()]
        rec["best"] = max(grid.values())
        heatmap(b, grid, a.step, os.path.join(outd, gid + ".png"))
    finally:
        cleanup(st, scratch)
    json.dump(rec, open(res, "w"))
    return rec

def targets(rec, min_change):
    """The distinct results of the high-change taps: points grouped by their exact change
    (one button gives one picture), each with its most central point."""
    groups = {}
    for x, y, f in rec.get("grid", []):
        if f >= min_change:
            groups.setdefault(f, []).append((x, y))
    out = []
    for f, pts in groups.items():
        cx, cy = sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)
        rep = min(pts, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)
        out.append(dict(change=f, points=len(pts), x=rep[0], y=rep[1]))
    return out

def verify(g, a, outd, tmp):
    """For a screen with more than one high-change result: does a result go back to a
    screen the mash run had already shown (and the stuck screen is not)? Compared by
    the screens' hashes, against every frame of the sweep's mash log."""
    res = os.path.join(outd, g["id"] + ".json")
    rec = json.load(open(res))
    ts = targets(rec, a.min_change)
    if len(ts) < 2 or all("back" in t for t in rec.get("targets", [])) and len(rec.get("targets", [])) == len(ts):
        return rec
    hist = [l.split("\t")[1:3] for l in open(os.path.join(a.sweep, g["set"], "logs", "mash", g["id"] + ".tsv")).read().split("\n")[1:] if l]
    pairs, bots, tops = {tuple(h) for h in hist}, {h[1] for h in hist}, {h[0] for h in hist}
    base, st, rom, scratch, _ = state(g, a, tmp)
    try:
        def hashes(touch):
            hf = os.path.join(tmp, f"{g['id']}.v.h")
            cmd = base + ["--load-state", st, "--frames", str(a.wait + 1), "--hash-frames", hf]
            if touch:
                cmd += ["--touch", f"1:{touch[0]},{touch[1]}:{a.hold}"]
            run(cmd + [rom])
            try:
                return [tuple(l.split()[1:3]) for l in open(hf) if l.strip()]
            finally:
                if os.path.exists(hf): os.remove(hf)
        stuck = set(hashes(None))
        for t in ts:
            hs = hashes((t["x"], t["y"]))
            end = hs[-1] if hs else None
            new = [h for h in hs if h not in stuck]
            t["back"] = bool(end and end in pairs and end not in stuck)
            t["back_bottom"] = bool(end and end[1] in bots and end[1] not in {s[1] for s in stuck})
            t["seen_before"] = sum(1 for h in new if h in pairs)
            t["new_frames"] = len(new)
        rec["targets"] = ts
    finally:
        cleanup(st, scratch)
    json.dump(rec, open(res, "w"))
    return rec

def plan(a):
    recs = []
    for s in sorted(os.listdir(os.path.join(a.sweep, "probe"))):
        d = os.path.join(a.sweep, "probe", s)
        recs += [json.load(open(os.path.join(d, f))) for f in os.listdir(d) if f.endswith(".json")]
    moved = {}   # (x, y) -> games it moves on
    back = 0
    for r in recs:
        backf = {t["change"] for t in r.get("targets", []) if t.get("back") or t.get("back_bottom")}
        back += len(backf)
        for x, y, f in r.get("grid", []):
            if f >= a.min_change and f not in backf:
                moved.setdefault((x, y), set()).add(r["file"])
    can = set().union(*moved.values()) if moved else set()
    print(f"{back} tap results left out as going back to an earlier screen")
    print(f"{len(recs)} games probed; {len(can)} have a point that changes >= {a.min_change:.0%} of the picture")
    left, picks = set(can), []
    while left and len(picks) < a.points:
        p = max(moved, key=lambda p: (len(moved[p] & left), -abs(p[0] - 128) - abs(p[1] - 96)))
        got = moved[p] & left
        if not got:
            break
        picks.append(p); left -= got
        print(f"  tap {p[0]:3d},{p[1]:3d}: +{len(got):3d} (total {len(can) - len(left)})  e.g. {', '.join(sorted(got)[:3])}")
    print(f"not moved by any of them: {len(left)} of {len(can)}; no point moves: {len(recs) - len(can)}")
    for r in sorted(recs, key=lambda r: r["file"]):
        if r["file"] not in can:
            print(f"    - {r['file']} (best {r.get('best', r.get('error'))})")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sweep", required=True)
    ap.add_argument("--set", action="append", default=[], help="NAME=DIR: the ROM set of the sweep's NAME output")
    ap.add_argument("--names")
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--verify", action="store_true", help="after a probe: check screens with several high-change results for taps that go back")
    ap.add_argument("--points", type=int, default=8)
    ap.add_argument("--min-change", type=float, default=0.05)
    ap.add_argument("--step", type=int, default=8)
    ap.add_argument("--hold", type=int, default=4)
    ap.add_argument("--wait", type=int, default=60)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--bios", default=os.environ.get("DS_BIOS"))
    ap.add_argument("--dsi-bios", default=os.environ.get("DS_DSI_BIOS"))
    ap.add_argument("--headless", default=os.path.join(REPO, "build/host/src/frontend/headless/dsperate-headless"))
    a = ap.parse_args()
    a.sweep = os.path.abspath(a.sweep); a.dsi_mode = False
    if a.plan:
        return plan(a)
    a.sets = dict(s.split("=", 1) for s in a.set)
    text = open(a.names).read()
    games = []
    for s in a.sets:
        for l in open(os.path.join(a.sweep, s, "results-mash.jsonl")):
            r = json.loads(l)
            if os.path.splitext(r["file"])[0] in text:
                games.append(dict(r, set=s))
    print(f"{len(games)} games to {'verify' if a.verify else 'probe'}", flush=True)
    with tempfile.TemporaryDirectory(prefix="probe-", dir=a.sweep) as tmp, cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {}
        for g in games:
            outd = os.path.join(a.sweep, "probe", g["set"]); os.makedirs(outd, exist_ok=True)
            if a.verify and not os.path.exists(os.path.join(outd, g["id"] + ".json")):
                continue
            futs[ex.submit(verify if a.verify else probe, g, a, outd, tmp)] = g
        for n, f in enumerate(cf.as_completed(futs), 1):
            g = futs[f]
            try:
                r = f.result()
                if a.verify:
                    ts = r.get("targets", [])
                    msg = ", ".join(f"{t['x']},{t['y']} {t['change']:.0%}{' BACK' if t['back'] else ' back?' if t['back_bottom'] else ''}" for t in ts) or "-"
                    print(f"[{n}/{len(futs)}] {g['file']}: {msg}", flush=True); continue
                msg = f"best {r.get('best')}" + ("" if r.get("rerun_matches", True) else " (RERUN DIFFERS)") + (f" {r['error']}" if "error" in r else "")
            except Exception as e:
                msg = f"ERROR {e!r}"
            print(f"[{n}/{len(futs)}] {g['file']}: {msg}", flush=True)
        for s in a.sets:
            if os.path.exists(os.path.join(a.sets[s], ".dsperate")):
                sys.exit(f"STOPPED: something wrote {a.sets[s]}/.dsperate")

if __name__ == "__main__":
    main()
