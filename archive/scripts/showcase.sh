#!/usr/bin/env bash
# Builds the static results page into site/. Usage: scripts/showcase.sh [pm-record-dir]
set -euo pipefail
cd "$(dirname "$0")/.."
out=site
mkdir -p "$out"
args=(--out "$out")
if [ -x build/release/bench/hft_bench ]; then
  build/release/bench/hft_bench --benchmark_filter='EventStep|Fork|Philox' --benchmark_min_time=0.1s \
    --benchmark_format=json >"$out/bench.json"
  args+=(--bench "$out/bench.json")
fi
if [ -n "${1:-}" ]; then
  build/release/apps/pm_stats "$1" >"$out/tokens.tsv"
  args+=(--pm-tsv "$out/tokens.tsv" --pm-dir "$1")
fi
${PYTHON:-python3} research/showcase.py "${args[@]}"
