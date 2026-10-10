#!/usr/bin/env bash
# E6.3: makers and the arbitrage executor on a recording at the measured round trip (182 ms) and
# at 50, 150 and 300 ms (D4); one-way latency is half. One TSV row per setting.
B=${PM_EXEC:-$(dirname "$0")/../build/native/apps/pm_exec}
C=$(dirname "$0")/../config/exec.cfg
R=${1:-~/data/pm-live/20261009T160027Z}
keys="maker_fills taker_fills arb_windows arb_filtered arb_attempts arb_complete arb_empty arb_completed_after_leg_loss arb_unwound arb_frozen arb_residual_groups arb_merges arb_splits arb_realised_usd arb_sets_held fees_usd ledger_pnl_usd capital_usd_days kill decision_hash_v2"
printf "rtt_ms"; for k in $keys; do printf "\t%s" "$k"; done; echo
for rtt in 182 50 150 300; do
  line=$($B --replay $R --config $C --set arb=1 --set lat_ms=$((rtt / 2)) 2>&1 >/dev/null | grep "^exec")
  printf "%s" "$rtt"
  for k in $keys; do printf "\t%s" "$(echo "$line" | grep -o "\b$k [^ ]*" | cut -d" " -f2)"; done
  echo
done
