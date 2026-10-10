#!/usr/bin/env bash
# Demo (c): leg risk. With 91 ms latency and dropped acks, legs miss; every incomplete set is
# completed or unwound within the attempt bound, or frozen and counted; none is left silent.
exec "$(dirname "$0")/run.sh" 2d532f7739e2ba18 "arb_attempts arb_complete arb_completed_after_leg_loss arb_unwound arb_frozen arb_residual_groups arb_realised_usd kill" \
  --replay ~/data/pm-live/20261009T160027Z --config config/exec.cfg --set arb=1 --set p_drop_ack=0.3
