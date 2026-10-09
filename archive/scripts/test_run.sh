#!/bin/bash
# The one held-out run of MS11 (DESIGN.md section 4, protocol). Opens the locked test days once.
# Run only after the strategies, parameters and report code are frozen at a tagged commit.
# Usage: scripts/test_run.sh <frozen-tag>
# Needs: the test-day stores ingested (scripts/ingest_day.sh) and the policies in $HFT_DATA/policies.
# Env: HFT_DATA (default ~/data), read by the research scripts.
set -euo pipefail

tag=${1:?usage: test_run.sh <frozen-tag>}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[ "$(git rev-parse HEAD)" = "$(git rev-parse "$tag^{commit}")" ] || { echo "HEAD is not $tag"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "working tree not clean"; exit 1; }
py=${PYTHON:-python}
export HFT_DATA=${HFT_DATA:-$HOME/data}

echo "== lock audit before the run"
"$py" research/audit.py

days=$("$py" -c "import tomllib; print(' '.join(tomllib.load(open('configs/splits.toml','rb'))['test']['days']))")
run() { "$py" research/backtests.py build/py --days $days --allow-test --question Q1 --workers 6 "$@"; }
run --name test-main --strategies naive,as,dp,dp_signal,ext
run --name test-latency --strategies ext --latency-us 0,10,50,100,250,500
run --name test-ablation --strategies ext,ext_no_toxicity,ext_no_taking,dp_signal,dp
run --name test-sens --strategies naive,as,dp,dp_signal,ext --fees cap30,cap10 --fill-rule queue,trade_through
run --name test-sanity --strategies zero,random_taker,random_passive,foresight
"$py" research/backtest_report.py --main test-main --latency test-latency --sensitivity test-sens \
    --ablation test-ablation --sanity test-sanity --out docs/results/ms11-test.md
echo "== test runs are now in the registry with test_access = 1"
