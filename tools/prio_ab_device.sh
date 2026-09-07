#!/bin/sh
# Thread-priority experiment (no code): SDL frontend, fullscreen dual-window on
# Wayland, audio on, vsync on. Variants applied with chrt 8 s into the run.
cd /storage/dsperate/ab || exit 1
export XDG_RUNTIME_DIR=/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 DS_FRAME_STATS=1
ROUNDS=${1:-2}
run() { # variant scene
  v=$1; s=$2
  case $s in
    etody) A="--save scenes/etody.sav --replay scenes/etody.dsin"; R="/storage/roms/nds/Etrian Odyssey.nds";;
    gsdd)  A="--load-state scenes/gsdd-phase2.dss"; R="/storage/roms/nds/Golden Sun - Dark Dawn.nds";;
    sm64)  A="--save scenes/sm64.sav --replay scenes/sm64.dsin"; R="/storage/roms/nds/Super Mario 64 DS.nds";;
  esac
  ini=/storage/.config/dsperate/dsperate.ini; [ $v = off ] && ini=rt-off.ini
  ./dsperate.noharden --config $ini --fullscreen --dual-window $A --frames 1800 --stats-from 600 "$R" > /tmp/prio-$v-$s.log 2>&1 &
  sleep 8
  P=$(pidof dsperate.noharden)
  if [ -n "$P" ]; then
    aud=$(for t in /proc/$P/task/*; do [ "$(cat $t/comm)" = SDLAudioP0 ] && basename $t; done)
    lw=""; bw=""
    for t in $(ls /proc/$P/task | sort -n); do
      [ $t = $P ] && continue
      [ "$(cat /proc/$P/task/$t/comm)" = "${P}" ] && :
      case "$(cat /proc/$P/task/$t/comm)" in dsperate*) if [ -z "$lw" ]; then lw=$t; else bw="$bw $t"; fi;; esac
    done
    case $v in
      audio6)   chrt -r -p 6 $aud;;
      bwother)  for t in $bw; do chrt -o -p 0 $t; done;;
      both)     chrt -r -p 6 $aud; for t in $bw; do chrt -o -p 0 $t; done;;
      emu6)     chrt -r -p 6 $P; chrt -r -p 6 $aud;;
    esac
    echo "  [$v $s] audio=$aud lw=$lw bw=$bw" >&2
  fi
  wait
  printf '%-8s %-6s ' $v $s
  grep -E '^work ms' /tmp/prio-$v-$s.log | sed 's/work ms: //; s/mean [0-9.]* //; s/min.*//'
  grep -q 'video: dmabuf' /tmp/prio-$v-$s.log || echo "   !! not dmabuf"
}
r=1
while [ $r -le $ROUNDS ]; do
  for s in etody gsdd; do
    for v in rr5 off audio6 bwother both emu6; do run $v $s; done
  done
  r=$((r+1))
done
