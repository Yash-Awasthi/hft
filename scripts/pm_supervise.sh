#!/usr/bin/env bash
# Keeps pm_record running: restarts it after any exit except the size cap (exit 3) or a
# deliberate stop. Usage: scripts/pm_supervise.sh <data-dir> [pm_record options]
set -u
trap '' HUP  # survive the session that started it
dir=${1:?data dir}
shift
bin=${PM_RECORD_BIN:-"$(cd "$(dirname "$0")/.." && pwd)/build/release/apps/pm_record"}
mkdir -p "$dir"
# One supervisor per directory: a second start (another logon) exits at once.
exec 9>"$dir/.supervisor.lock"
flock -n 9 || { echo "already running for $dir" >&2; exit 0; }
while true; do
  "$bin" --out "$dir" "$@"
  rc=$?
  echo "$(date -u +%FT%TZ) pm_record exited with $rc" >>"$dir/supervisor.log"
  [ "$rc" -eq 3 ] || [ "$rc" -eq 130 ] || [ "$rc" -eq 143 ] && exit "$rc"
  sleep 5
done
