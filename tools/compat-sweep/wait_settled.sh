#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Wait until DIR stops changing, then run a command: for a set still being copied in.
#   wait_settled.sh DIR [-- CMD ARGS...]
# Settled = QUIET_SECS (default 900) with no change in file count, total bytes or
# newest mtime, polled every POLL seconds (default 60). Read-only on DIR.
set -u
dir=${1:?usage: wait_settled.sh DIR [-- CMD ARGS...]}; shift
[ "${1:-}" = -- ] && shift
QUIET_SECS=${QUIET_SECS:-900}; POLL=${POLL:-60}
snap() { find "$dir" -type f -printf '%s %T@\n' | awk '{n++; b+=$1; if ($2>m) m=$2} END {printf "%d %d %.0f", n, b, m}'; }
last=""; since=$(date +%s)
echo "$(date '+%F %T') watching $dir (quiet ${QUIET_SECS}s, poll ${POLL}s)"
while :; do
  cur=$(snap); now=$(date +%s)
  if [ "$cur" != "$last" ]; then echo "$(date '+%F %T') files/bytes/newest: $cur"; last=$cur; since=$now
  elif [ $((now - since)) -ge "$QUIET_SECS" ]; then echo "$(date '+%F %T') settled: $cur"; break; fi
  sleep "$POLL"
done
[ $# -gt 0 ] && exec "$@"
