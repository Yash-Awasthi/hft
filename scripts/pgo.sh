#!/usr/bin/env bash
# Profile-guided release build. Trains on a real store (per-symbol and merged replay) and
# writes the optimised binaries to build/pgo-use. With a pm_record directory it also trains
# the prediction-market tools on it. Usage: scripts/pgo.sh <store-dir> [pm-record-dir]
set -euo pipefail
store=${1:?store directory}
pm=${2:-}
cd "$(dirname "$0")/.."
rm -rf build/pgo-profile build/pgo-gen build/pgo-use
cmake --preset pgo-gen
cmake --build build/pgo-gen --target book_study book_replay checkpoint hft_bench pm_stats pm_mm
b=build/pgo-gen
HFT_LOOKAHEAD=8 HFT_BIGPOOL=1 "$b/bench/book_study" "$store" replay 50 1 1 >/dev/null
HFT_BIGPOOL=1 "$b/bench/book_study" "$store" replay-sym 50 1 1 >/dev/null
"$b/apps/book_replay" "$store" tick >/dev/null
if [ -n "$pm" ]; then
  "$b/apps/pm_stats" "$pm" >/dev/null
  "$b/apps/pm_mm" "$pm" >/dev/null
fi
cmake --preset pgo-use
cmake --build build/pgo-use
