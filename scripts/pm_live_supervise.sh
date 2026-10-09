#!/usr/bin/env bash
# Keeps pm_live running with a fresh recording directory per run: <data-dir>/<UTC start>/.
# Restarts after any exit except a deliberate stop (130, 143).
# Usage: scripts/pm_live_supervise.sh <data-dir> [pm_live options]
set -u
dir=${1:?data dir}
shift
bin="$(cd "$(dirname "$0")/.." && pwd)/build/native/apps/pm_live"
mkdir -p "$dir"
while true; do
  run="$dir/$(date -u +%Y%m%dT%H%M%SZ)"
  "$bin" --record "$run" "$@" >"$run.summary.tsv" 2>"$run.log"
  rc=$?
  echo "$(date -u +%FT%TZ) pm_live exited with $rc" >>"$dir/supervisor.log"
  [ "$rc" -eq 130 ] || [ "$rc" -eq 143 ] && exit "$rc"
  sleep 10
done
