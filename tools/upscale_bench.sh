#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The upscaled-3D baseline on the RG DS Plus: every scene of tools/scenes.sh
# under each video configuration, in the SDL frontend as ROCKNIX launches it,
# twice (forward, then in reverse order: the first run of a pair runs slow).
#
#   tools/upscale_bench.sh push BIN          # copy BIN, the scene files and a test config to the device
#   tools/upscale_bench.sh run [CONFIGS]     # start the batch on the device (detached); default all
#   tools/upscale_bench.sh fetch DIR         # copy the logs back
#   tools/upscale_bench.sh table DIR         # one line per scene x config from the logs
#   tools/upscale_bench.sh timeline DIR      # who had the GPU (tools/gpu_timeline.py), for a batch run with TL=1
#
# The table's frame times are emulation + present (what reaches the panel); the emulation thread's own are beside them.
# CONFIGS (comma-separated): cpu (the CPU raster at 1x), g2, g4 (the Vulkan
# raster at 2x / 4x), g2aa, g4aa (with MSAA). Environment on every run:
# DS_FRAME_STATS=1 DS_GPU_TRACE=200 DS_VK_TIMING=2 (stage GPU times).
# The device's own config with saves and states moved into the test directory
# (the batch never reads or writes the real ones), essway stopped, caches
# dropped before each run. SCENES (comma-separated) limits the run, PASSES (default "fwd rev") the passes; TL=1 also
# records each run's GPU timeline and frame series (DS_GPU_TIMELINE, DS_FRAME_SERIES). DEV (ssh host, default rgdsplus), DIR (default
# /storage/dsperate-test/upscale).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
source "$HERE/tools/scenes.sh"
DEV=${DEV:-rgdsplus}
T=${DIR:-/storage/dsperate-test/upscale}
ALL=cpu,g2,g4,g2aa,g4aa
# A ROM the device has differently from the one a scene was recorded with: the local one goes along.
LOCAL_ROMS=("Mario & Luigi - Bowser's Inside Story.nds")
ROMS=${DS_ROMS:-$HERE/../resources/ds/games}

cfg_flags() {
  case $1 in
    cpu) echo "--no-gpu3d --no-aa" ;;
    g2) echo "--gpu3d --gpu3d-scale 2 --no-aa" ;;
    g4) echo "--gpu3d --gpu3d-scale 4 --no-aa" ;;
    g2aa) echo "--gpu3d --gpu3d-scale 2 --aa" ;;
    g4aa) echo "--gpu3d --gpu3d-scale 4 --aa" ;;
    *) echo "unknown config $1" >&2; exit 2 ;;
  esac
}

case ${1:-} in
  push)
    bin=${2:?BIN}
    ssh "$DEV" "mkdir -p '$T/scenes' '$T/saves' '$T/states' '$T/roms' '$T/logs'"
    scp -q "$bin" "$DEV:$T/dsperate"
    scp -q "$HERE"/scenes/*.dsin "$HERE"/scenes/*.sav "$HERE"/scenes/*.dss "$DEV:$T/scenes/"
    for r in "${LOCAL_ROMS[@]}"; do scp -q "$ROMS/$r" "$DEV:$T/roms/"; done
    ssh "$DEV" "sed -e 's|^saves = .*|saves = $T/saves|' -e 's|^states = .*|states = $T/states|' /storage/.config/dsperate/dsperate.ini > '$T/test.ini'"
    echo "pushed to $DEV:$T" ;;
  run)
    configs=${2:-$ALL}
    jobs=$(mktemp)
    for pass in ${PASSES:-fwd rev}; do
      list=()
      for s in "${UPSCALE_SCENES[@]}"; do
        name=${s%%|*}
        [ -n "${SCENES:-}" ] && [[ ",$SCENES," != *",$name,"* ]] && continue
        for c in ${configs//,/ }; do list+=("$name|$c"); done
      done
      if [ $pass = rev ]; then mapfile -t list < <(printf '%s\n' "${list[@]}" | tac); fi
      for e in "${list[@]}"; do
        name=${e%%|*}; c=${e##*|}
        frames=$(scene_field "$name" 2); from=$(scene_field "$name" 3); rom=$(scene_field "$name" 6)
        rp=/storage/roms/nds
        for r in "${LOCAL_ROMS[@]}"; do [ "$r" = "$rom" ] && rp=$T/roms; done
        romq=$(printf '%s' "$rp/$rom" | sed "s/'/'\\\\''/g")
        args=$(scene_args "$name" "$T/scenes" | { grep -vx -- '--guitar-grip' || true; } | sed "s/'/'\\\\''/g; s/.*/'&'/" | tr '\n' ' ')   # the SDL frontend fits the grip itself
        tl=; [ -n "${TL:-}" ] && tl="DS_GPU_TIMELINE=logs/$name.$c.$pass.tl DS_FRAME_SERIES=logs/$name.$c.$pass.ser "
        echo "sync; echo 3 > /proc/sys/vm/drop_caches; ${tl}DS_FRAME_STATS=1 DS_GPU_TRACE=200 DS_VK_TIMING=2 ./dsperate --fullscreen --dual-window --config test.ini --frames $frames --stats-from $from $(cfg_flags "$c") $args '$romq' > logs/$name.$c.$pass.log 2>&1; echo \"rc=\$?\" >> logs/$name.$c.$pass.log" >> "$jobs"
      done
    done
    scp -q "$jobs" "$DEV:$T/jobs.sh"; rm -f "$jobs"
    ssh "$DEV" "cat > '$T/batch.sh'" <<EOF
#!/bin/sh
# One batch: essway stopped for its length and restored after.
cd '$T'
. /etc/profile
was=\$(systemctl is-active essway)
[ "\$was" = active ] && systemctl stop essway
n=\$(wc -l < jobs.sh); i=0
while IFS= read -r line; do i=\$((i+1)); echo "\$i/\$n" > progress; sh -c "\$line"; done < jobs.sh
echo done > progress
[ "\$was" = active ] && systemctl start essway
EOF
    ssh -n -f "$DEV" "cd '$T' && setsid nohup sh batch.sh > batch.out 2>&1 < /dev/null & exit 0"
    echo "started on $DEV ($(ssh "$DEV" "wc -l < '$T/jobs.sh'") runs); progress: ssh $DEV cat $T/progress" ;;
  fetch)
    mkdir -p "${2:?DIR}"; scp -q "$DEV:$T/logs/*" "$2/" && ls "$2" | wc -l ;;
  timeline)
    h=--header
    for f in "${2:?DIR}"/*.tl; do n=$(basename "$f" .tl); python3 "$HERE/tools/gpu_timeline.py" "$f" "${f%.tl}.ser" --name "${n%%.*}" $h; h=; done ;;
  table)
    python3 - "${2:?DIR}" <<'PY'
import glob, os, re, sys, collections
rows = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.log'))):
    scene, cfg, run = os.path.basename(f)[:-4].split('.')
    t = open(f, errors='replace').read()
    m = re.search(r'^frame ms: median ([\d.]+) mean [\d.]+ p90 [\d.]+ p95 [\d.]+ p99 ([\d.]+) max ([\d.]+)', t, re.M)
    b = re.search(r'^frame budget: (\d+) of (\d+) frames over', t, re.M)
    wm = re.search(r'^work ms: median ([\d.]+) mean [\d.]+ p90 [\d.]+ p95 [\d.]+ p99 ([\d.]+) max ([\d.]+)', t, re.M)
    wb = re.search(r'^work budget: (\d+) of (\d+) frames over', t, re.M)
    g = re.findall(r'gpu ([\d.]+) ms \((\d+) timed\)', t)
    st = re.findall(r'gpu3d stages \(ms/frame, worst\): rows ([\d.]+) \(([\d.]+)\), main pass ([\d.]+) \(([\d.]+)\), edge mark ([\d.]+) \(([\d.]+)\), down\+copy ([\d.]+) \(([\d.]+)\)', t)
    rc = re.search(r'rc=(\d+)', t)
    rows[(scene, cfg)].append(dict(run=run, med=wm and float(wm[1]), p99=wm and float(wm[2]), max=wm and float(wm[3]),
        over=wb and int(wb[1]), n=wb and int(wb[2]), emed=m and float(m[1]), eover=b and int(b[1]), gpu=sum(float(x[0]) for x in g) / len(g) if g else None,
        st=[sum(float(s[i]) for s in st) / len(st) for i in (0, 2, 4, 6)] if st else None, rc=rc and rc[1]))
# median, p99, max, over: the frame as displayed (emulation + present, the log's "work" series). The emu columns are the
# emulation thread alone, which a slow GPU raster does not hold up: its frames go late at the present instead.
print(f"{'scene':11} {'cfg':5} {'run':4} {'median':>7} {'p99':>6} {'max':>6} {'over':>9} {'emu med':>7} {'emu over':>8} {'gpu':>6}  stages rows/main/mark/rest  rc")
for (scene, cfg), rs in sorted(rows.items()):
    for r in rs:
        st = '/'.join(f"{x:.2f}" for x in r['st']) if r['st'] else '-'
        f = lambda v, p=2: f"{v:.{p}f}" if v is not None else '-'
        print(f"{scene:11} {cfg:5} {r['run']:4} {f(r['med']):>7} {f(r['p99']):>6} {f(r['max']):>6} {str(r['over'])+'/'+str(r['n']):>9} {f(r['emed']):>7} {str(r['eover']):>8} {f(r['gpu']):>6}  {st:26} {r['rc']}")
PY
    ;;
  *) sed -n 3,18p "$0"; exit 2 ;;
esac
