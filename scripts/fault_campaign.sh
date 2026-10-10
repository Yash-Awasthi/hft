#!/usr/bin/env bash
# G3a: each core fault on the reference recording. A run must end with zero illegal reports and
# either no position mismatch or a kill switch naming the mismatch: never silently wrong.
B=${PM_LIVE:-$(dirname "$0")/../build/native/apps/pm_live}
R=${1:-~/data/pm-live/20261009T160027Z}
base="--venue-compat --lat-ms 50 --jitter-ms 20 --k 3000 --gamma 0.01 --size 10"
for f in "" "--drop-ack 0.2" "--drop-fill 0.2" "--dup 0.2" "--settle-fail 0.2" "--disconnect 120:8"; do
  out=$($B --replay $R $base $f --seed 7 2>&1 >/dev/null)
  echo "[${f:-none}]"
  echo "$out" | grep -o "paper_fills [0-9]*\|decision_hash [0-9a-f]*" | tr "\n" " "; echo
  echo "$out" | grep "^venue" | sed "s/^venue //"
done
