#!/usr/bin/env bash
# Maker parity (G3b): own fill model vs order manager + simulated venue in compat mode.
B=${PM_LIVE:-$(dirname "$0")/../build/native/apps/pm_live}
for R in ~/data/pm-live/2*/; do
  [ -f "$R/session.tsv" ] || continue
  for args in "" "--k 3000 --gamma 0.01 --size 10"; do
    a=$($B --replay $R $args --dump-fills ${TMPDIR:-/tmp}/pa.txt 2>&1 >/dev/null | grep -o "decision_hash [0-9a-f]*" | tail -1)
    b=$($B --replay $R $args --venue-compat --dump-fills ${TMPDIR:-/tmp}/pb.txt 2>&1 >/dev/null | tee ${TMPDIR:-/tmp}/pb.err | grep -o "decision_hash [0-9a-f]*" | tail -1)
    n=$(wc -l < ${TMPDIR:-/tmp}/pa.txt)
    if [ "$a" = "$b" ] && cmp -s ${TMPDIR:-/tmp}/pa.txt ${TMPDIR:-/tmp}/pb.txt; then r=SAME; else r=DIFF; fi
    echo "$(basename $R) [$args] fills $n $a | $r | $(grep "^venue" ${TMPDIR:-/tmp}/pb.err)"
  done
done
