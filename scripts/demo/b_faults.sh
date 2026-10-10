#!/usr/bin/env bash
# Demo (b): core faults on (dropped acks, duplicates, failed settlements, disconnects): orders go
# Unknown and are reconciled, the ledger matches the venue, nothing illegal, no kill.
exec "$(dirname "$0")/run.sh" 0fa64bdff00983cc "unknown_timeouts settle_failed illegal_reports oms_mismatches position_mismatches kill" \
  --replay ~/data/pm-live/20261009T160027Z --config config/exec.cfg \
  --set p_drop_ack=0.2 --set p_dup=0.2 --set p_settle_fail=0.2 --set disconnect_every_s=120 --set disconnect_for_s=8
