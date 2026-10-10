#!/usr/bin/env bash
# Demo (a): clean replay of the reference recording, makers and the arbitrage executor on.
exec "$(dirname "$0")/run.sh" 3c8be7fa30920873 "orders maker_fills taker_fills arb_attempts arb_complete arb_merges ledger_pnl_usd kill" \
  --replay ~/data/pm-live/20261009T160027Z --config config/exec.cfg --set arb=1
