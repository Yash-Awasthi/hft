#!/bin/bash
# Full-day checks on local data, the nightly tier of DESIGN.md section 12.
# Usage: scripts/nightly.sh <store-dir> [out-dir]
# Env: HFT_BUILD (default build/release), ITCHFEED_DIR (unpacked itchfeed wheel; the
#      third-party comparison is skipped without it), FUZZ_SECONDS (default 600).
set -euo pipefail

store=${1:?usage: nightly.sh <store-dir> [out-dir]}
out=${2:-$store/nightly}
bin=${HFT_BUILD:-$(dirname "$0")/../build/release}
root=$(dirname "$0")/..
mkdir -p "$out"
fail=0
step() { echo "== $1"; }

step "differential: every book against std::map on all symbols"
"$bin/apps/book_replay" "$store" map > "$out/map.txt" || fail=1
for b in tick tick-rh tick-dm tick-aos tick-soa tick-sv; do
    "$bin/apps/book_replay" "$store" $b > "$out/$b.txt" || fail=1
    cmp -s "$out/map.txt" "$out/$b.txt" && echo "$b identical" || { echo "$b DIFFERS"; fail=1; }
done

step "invariants after every event on symbols under 2000 live orders, every 1000 on all"
small=$(awk 'NR>1 && $6<2000 {print $1}' "$out/map.txt")
CHECK_EVERY=1 "$bin/apps/book_replay" "$store" tick $small > "$out/check1.txt" || fail=1
CHECK_EVERY=1000 "$bin/apps/book_replay" "$store" tick > "$out/check1000.txt" || fail=1

step "independent Python reference on 200 sampled symbols plus the busiest"
python3 "$root/research/compare_ref.py" "$store" "$out/tick.txt" 200 "$(date +%j)" \
    > "$out/ref.txt" || fail=1
tail -1 "$out/ref.txt"

step "checkpoints"
"$bin/apps/checkpoint" "$store" --verify || fail=1

step "store integrity: merged stream against stream.sha256 (recorded on the first run)"
got=$("$bin/apps/store_cat" "$store" | sha256sum | cut -d' ' -f1)
if [ -f "$store/stream.sha256" ]; then
    [ "$got" = "$(cat "$store/stream.sha256")" ] && echo "stream hash ok" ||
        { echo "stream hash DIFFERS"; fail=1; }
else
    echo "$got" > "$store/stream.sha256"
    echo "recorded stream hash $got"
fi

if [ -n "${ITCHFEED_DIR:-}" ]; then
    step "third-party decoder comparison on 2M messages from a random offset"
    total=$(awk 'NR>1 {s+=$3} END{print s}' "$out/map.txt")
    skip=$(( (RANDOM * 32768 + RANDOM) % (total - 2000000) ))
    "$bin/apps/store_cat" "$store" |
        "$bin/apps/itch_dump" $skip 2000000 "$out/slice.bin" > "$out/ours.txt" || fail=1
    python3 -I "$root/research/compare_itchfeed.py" "$ITCHFEED_DIR" "$out/slice.bin" \
        "$out/ours.txt" | tail -1 || fail=1
    rm -f "$out/slice.bin"
fi

if [ -x "$root/build/fuzz/fuzz/itch_fuzz" ]; then
    step "long fuzzing"
    mkdir -p "$out/corpus"
    "$root/build/fuzz/fuzz/itch_fuzz" -max_total_time="${FUZZ_SECONDS:-600}" "$out/corpus" \
        2>&1 | tail -2 || fail=1
fi

echo "== nightly $([ $fail = 0 ] && echo passed || echo FAILED)"
exit $fail
