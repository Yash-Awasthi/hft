#!/usr/bin/env bash
# Demo (d): kill switch. A paper run tripped by its KILL file 20 s in (the recording carries the
# trip); the replay cancels every order, places none after and writes kill.log.
out=${TMPDIR:-/tmp}/demo-kill
rm -rf "$out"
"$(dirname "$0")/run.sh" 76e7e15544ecbb02 "orders risk_rejects open_orders kill" \
  --replay ~/data/exec/runs/demo-kill --run "$out" || exit 1
head -3 "$out/kill.log"
