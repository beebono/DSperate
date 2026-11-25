#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility sweep runner: one headless run per game, in parallel, resumable.

  run_sweep.py --set DIR --list titles.tsv --out OUT --phase idle|mash|jit [options]

DS cartridges run as a DS, DSi cartridges and DSiWare (CDN layout: the newest
version's app, see romsets.py) as a DSi; --dsi-mode runs DS cartridges as a DSi.

Phases (see README.md):
  idle  no input for --frames frames: boot, title, attract mode
  mash  the same length with the standard synthetic input (A, Start and a tap on
        the bottom screen, one at a time, never together, no B)
  jit   the AArch64 JIT under qemu in strict mode against the host interpreter,
        both on exact timing with exact frameskip: screen hashes must be equal.
        --jit-input idle|mash picks the input; --frames is the length.

The set is only read: each zipped ROM is unpacked into a scratch directory
under OUT for its run and deleted after (the headless's own zip cache would
write <set>/.dsperate/), no save is written (no --save-out), and every output
goes under OUT. Results are appended to
OUT/results-<phase>.jsonl; a game already there is skipped, so a run resumes.
"""
import argparse, concurrent.futures as cf, json, os, re, shutil, subprocess, sys, tempfile, threading, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from romsets import pick

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SHOTS_IDLE = (300, 1200, 2400, 3600)   # plus the last frame

def game_list(path, set_dir):
    with open(path) as fh:
        lines = fh.read().splitlines()
    if lines and lines[0].startswith("title\t") and "\tpick\t" in lines[0] + "\t":
        col = lines[0].split("\t").index("pick")
        files = [l.split("\t")[col] for l in lines[1:] if l.strip()]
    else:
        files = [l for l in lines if l.strip() and not l.startswith("#")]
    return [f for f in files if os.path.exists(os.path.join(set_dir, f))]

def game_id(rel):
    stem = os.path.splitext(os.path.basename(rel))[0]
    return re.sub(r"[^A-Za-z0-9._()+,-]+", "_", stem)[:150]

TAP_Y = (96, 72, 120, 48, 144)   # the bottom screen's centre column, where menus put their buttons

def write_mash(path, frames, start=120, period=40):
    """The standard mash, written by tools/mkdsin.py: A, Start, then a tap in the
    bottom screen's centre column (its height cycling through TAP_Y), 4 frames
    each, 12 frames apart, every `period` frames."""
    args = []
    for k, c in enumerate(range(start, frames, period)):
        args += [f"k{c}:1:4", f"k{c + 12}:8:4", f"{c + 24}:128,{TAP_Y[k % len(TAP_Y)]}:4"]
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "mkdsin.py"), path, str(frames)] + args, check=True)

def shots_png(raw, png, n):
    """--dump-frames output (n frames of both screens, 0xAARRGGBB) -> one strip PNG."""
    try:
        from PIL import Image
    except ImportError:
        return False
    fsz = 256 * 192 * 4 * 2
    data = open(raw, "rb").read()
    n = min(n, len(data) // fsz)
    if n == 0:
        return False
    strip = Image.new("RGB", (256 * n + 4 * (n - 1), 384), (40, 40, 40))
    for k in range(n):
        im = Image.frombuffer("RGBA", (256, 384), data[k * fsz:(k + 1) * fsz], "raw", "BGRA", 0, 1)
        strip.paste(im.convert("RGB"), (k * 260, 0))
    strip.save(png, optimize=True)
    return True

def run(cmd, env, timeout, log_path=None):
    t0 = time.monotonic()
    with open(log_path or os.devnull, "wb") as err:
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err, env=env, start_new_session=True)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, 9); p.wait(); rc = "timeout"
    return rc, round(time.monotonic() - t0, 2)

def tail(path, n=6):
    try:
        with open(path, errors="replace") as fh:
            return [l.rstrip() for l in fh.readlines()[-n:]]
    except OSError:
        return []

def first_diff(a, b):
    try:
        la, lb = open(a).read().splitlines(), open(b).read().splitlines()
    except OSError:
        return "missing"
    for x, y in zip(la, lb):
        if x != y:
            return int(x.split()[0])
    return None if len(la) == len(lb) else f"length {len(la)} vs {len(lb)}"

def extract(src, tmp, gid):
    """The ROM to run, unpacked into this job's own scratch directory, and its kind
    (romsets.py). The headless would cache a zip in <set>/.dsperate/
    (cart/zip_cache.h): a write into the set, and a race between parallel jobs."""
    if not src.lower().endswith(".zip"):
        ext = os.path.splitext(src)[1].lower()
        return src, None, "dsi-cart" if ext == ".dsi" else "ds-cart"
    d = os.path.join(tmp, gid); os.makedirs(d, exist_ok=True)
    with zipfile.ZipFile(src) as z:
        e = pick(z)
        dst = os.path.join(d, "rom" + e["ext"])
        with z.open(e["entry"]) as fi, open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo, 4 << 20)
    return dst, d, e["kind"]

def machine_args(kind, a):
    """BIOS, firmware and launch flags for a kind of dump. DS cartridges run as a DS
    (DSi-enhanced ones included) unless --dsi-mode; DSi cartridges and DSiWare as a
    DSi, DSiWare handed over as the DSi launcher does, on a synthesised NAND."""
    ds = ["--bios9", os.path.join(a.bios, "bios9.bin"), "--bios7", os.path.join(a.bios, "bios7.bin")]
    dsi = kind != "ds-cart" or a.dsi_mode
    if not dsi:
        return ds + ["--firmware", os.path.join(a.bios, "firmware.bin")]
    if not a.dsi_bios:
        raise LookupError(f"{kind} needs --dsi-bios")
    args = ds + ["--bios9i", os.path.join(a.dsi_bios, "dsi_bios9.bin"), "--bios7i", os.path.join(a.dsi_bios, "dsi_bios7.bin"),
                 "--firmware", os.path.join(a.dsi_bios, "dsi_firmware.bin"), "--dsi"]
    return args + (["--dsi-hle-launch"] if kind == "dsiware" else [])

def one(game, a, base_args, mash, tmp):
    gid = game_id(game)
    rec = {"file": game, "id": gid, "phase": a.phase, "frames": a.frames}
    try:
        rom, scratch, kind = extract(os.path.join(a.set, game), tmp, gid)
    except (zipfile.BadZipFile, LookupError, ValueError, OSError) as e:
        return dict(rec, rc="bad-zip", error=repr(e))
    try:
        return dict(run_one(game, gid, rom, a, machine_args(kind, a) + base_args, mash, tmp), kind=kind)
    except LookupError as e:
        return dict(rec, rc="setup", error=str(e))
    finally:
        if scratch:
            shutil.rmtree(scratch, ignore_errors=True)

def run_one(game, gid, rom, a, base_args, mash, tmp):
    rec = {"file": game, "id": gid, "phase": a.phase, "frames": a.frames}
    env = dict(os.environ)
    if a.phase in ("idle", "mash"):
        logd = os.path.join(a.out, "logs", a.phase); shotd = os.path.join(a.out, "shots", a.phase)
        os.makedirs(logd, exist_ok=True); os.makedirs(shotd, exist_ok=True)
        raw = os.path.join(tmp, gid + ".raw")
        at = [f for f in SHOTS_IDLE if f < a.frames] + [a.frames - 1]
        cmd = [a.headless] + base_args + ["--frames", str(a.frames), "--sweep-log", os.path.join(logd, gid + ".tsv"),
                                          "--dump-frames", raw, "--dump-at", ",".join(map(str, at))]
        if a.phase == "mash":
            cmd += ["--replay", mash]
        errp = os.path.join(logd, gid + ".err")
        rc, wall = run(cmd + [rom], env, a.timeout, errp)
        rec.update(rc=rc, wall=wall, shots=at)
        if os.path.exists(raw):
            shots_png(raw, os.path.join(shotd, gid + ".png"), len(at)); os.remove(raw)
        if rc != 0:
            rec["stderr"] = tail(errp)
        elif os.path.getsize(errp) < 64 << 10:
            os.remove(errp)   # keep stderr only when the run failed or said a lot
    else:   # jit
        jd = os.path.join(a.out, "jit"); os.makedirs(jd, exist_ok=True)
        env.update(DS_TIMING="exact", DS_IDLE_CUT="0")
        args = base_args + ["--frames", str(a.frames), "--frameskip", "59"]
        if a.jit_input == "mash":
            args += ["--replay", mash]
        hi, hj = os.path.join(tmp, gid + ".interp"), os.path.join(tmp, gid + ".jit")
        rci, wi = run([a.headless] + args + ["--hash-frames", hi, rom], env, a.timeout)
        envj = dict(env, DS_JIT_STRICT="1")
        rcj, wj = run(a.qemu.split() + [a.jit_headless] + args + ["--hash-frames", hj, rom], envj, a.timeout * 4,
                      os.path.join(jd, gid + ".err"))
        d = first_diff(hi, hj) if rci == 0 and rcj == 0 else "run failed"
        rec.update(rc_interp=rci, rc_jit=rcj, wall_interp=wi, wall_jit=wj, first_diff=d,
                   verdict="same" if d is None else "DIFF")
        if d is not None:   # keep both hash files for a closer look
            for p, ext in ((hi, ".interp.h"), (hj, ".jit.h")):
                if os.path.exists(p): os.replace(p, os.path.join(jd, gid + ext))
            rec["stderr_jit"] = tail(os.path.join(jd, gid + ".err"))
        else:
            for p in (hi, hj, os.path.join(jd, gid + ".err")):
                if os.path.exists(p): os.remove(p)
    return rec

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", required=True, help="the ROM set's directory (zips or .nds, any depth)")
    ap.add_argument("--list", required=True, help="titles.tsv from tabulate.py (its pick column), or one path per line relative to --set")
    ap.add_argument("--out", required=True)
    ap.add_argument("--phase", required=True, choices=("idle", "mash", "jit"))
    ap.add_argument("--frames", type=int, default=5400, help="frames per run (default 5400 = 90 s)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--bios", default=os.environ.get("DS_BIOS"), help="directory with bios9.bin, bios7.bin, firmware.bin (default $DS_BIOS)")
    ap.add_argument("--dsi-bios", default=os.environ.get("DS_DSI_BIOS"),
                    help="directory with dsi_bios9.bin, dsi_bios7.bin, dsi_firmware.bin, for DSi cartridges and DSiWare (default $DS_DSI_BIOS)")
    ap.add_argument("--dsi-mode", action="store_true", help="run DS cartridges as a DSi too (for the DSi-enhanced ones)")
    ap.add_argument("--headless", default=os.path.join(REPO, "build/host/src/frontend/headless/dsperate-headless"))
    ap.add_argument("--jit-headless", default=os.path.join(REPO, "build/aarch64/src/frontend/headless/dsperate-headless"))
    ap.add_argument("--qemu", default="qemu-aarch64-static -L /usr/aarch64-linux-gnu", help="runner for --jit-headless (empty on an AArch64 host)")
    ap.add_argument("--jit-input", choices=("idle", "mash"), default="mash")
    ap.add_argument("--timeout", type=int, default=600, help="seconds per run (the qemu JIT run gets 4x)")
    ap.add_argument("--only", help="regex on the file name")
    ap.add_argument("--exclude", default=r"\(Program\)", help="regex on the file name to leave out (default: factory test programs)")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--extra", default="", help="more headless arguments, space-separated")
    a = ap.parse_args()
    if not a.bios:
        sys.exit("need --bios or $DS_BIOS")
    a.set, a.out = os.path.abspath(a.set), os.path.abspath(a.out)
    if a.out.startswith(a.set + os.sep) or a.out == a.set:
        sys.exit("--out must not be inside the set")
    os.makedirs(a.out, exist_ok=True)
    games = game_list(a.list, a.set)
    if a.only:
        games = [g for g in games if re.search(a.only, g)]
    if a.exclude:
        games = [g for g in games if not re.search(a.exclude, g)]
    res_path = os.path.join(a.out, f"results-{a.phase}.jsonl")
    done = set()
    if os.path.exists(res_path):
        with open(res_path) as fh:
            done = {json.loads(l)["file"] for l in fh if l.strip()}
    todo = [g for g in games if g not in done][: a.limit]
    print(f"{len(games)} games, {len(done)} already done, running {len(todo)} with {a.jobs} jobs", flush=True)
    mash = os.path.join(a.out, f"mash-{a.frames}.dsin")
    if not os.path.exists(mash):
        write_mash(mash, a.frames)
    base = ["--direct"] + a.extra.split()
    lock = threading.Lock()
    t0, n = time.monotonic(), 0
    with tempfile.TemporaryDirectory(prefix="sweep-", dir=a.out) as tmp, open(res_path, "a") as res, \
            cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(one, g, a, base, mash, tmp): g for g in todo}
        for f in cf.as_completed(futs):
            try:
                rec = f.result()
            except Exception as e:   # a runner bug must not lose the rest of the sweep
                rec = {"file": futs[f], "id": game_id(futs[f]), "phase": a.phase, "rc": "runner-error", "error": repr(e)}
            if os.path.exists(os.path.join(a.set, ".dsperate")):
                ex.shutdown(cancel_futures=True)
                sys.exit(f"STOPPED: something wrote {a.set}/.dsperate; the set must stay untouched")
            with lock:
                res.write(json.dumps(rec) + "\n"); res.flush(); n += 1
                bad = rec.get("rc", 0) != 0 or rec.get("verdict") == "DIFF"
                if bad or n % 25 == 0:
                    el = time.monotonic() - t0
                    print(f"[{n}/{len(todo)} {el / 60:.0f} min, ~{el / n * (len(todo) - n) / 60:.0f} min left] "
                          f"{rec['id']}: {rec.get('verdict', rec.get('rc'))}", flush=True)

if __name__ == "__main__":
    main()
