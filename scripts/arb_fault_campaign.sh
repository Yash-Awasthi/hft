#!/usr/bin/env bash
# E5.3: the arbitrage executor under each core fault at a high rate. Every incomplete set must
# end completed, unwound or frozen (counted), never as a silent residual; zero illegal reports;
# a position mismatch only with the kill switch naming it.
B=${PM_EXEC:-$(dirname "$0")/../build/native/apps/pm_exec}
C=$(dirname "$0")/../config/exec.cfg
R=${1:-~/data/pm-live/20261009T160027Z}
for f in "" "p_drop_ack=0.3" "p_drop_fill=0.3" "p_dup=0.3" "p_settle_fail=0.3" "disconnect_every_s=120 disconnect_for_s=8"; do
  sets="--set arb=1"
  for kv in $f; do sets="$sets --set $kv"; done
  out=$($B --replay $R --config $C $sets 2>&1 >/dev/null | grep "^exec")
  echo "[${f:-none}] $(echo "$out" | grep -o "kill [a-z_]*\|illegal_reports [0-9]*\|position_mismatches [0-9]*\|arb_[a-z_]* [^ ]*\|exec_hash [0-9a-f]*" | grep -v "arb_filtered\|arb_windows\|arb_splits\|arb_held_cash" | tr "\n" " ")"
done
