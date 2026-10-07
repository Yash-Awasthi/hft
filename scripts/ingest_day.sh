#!/bin/bash
# Downloads one Nasdaq ITCH day, verifies it and ingests it into the per-symbol store.
# Usage: scripts/ingest_day.sh <remote-file-name>      e.g. S121225-v50.txt.gz
# Env: HFT_DATA (default ~/data), HFT_INGEST (default build/release/apps/ingest),
#      HFT_CHECKPOINT (default build/release/apps/checkpoint),
#      KEEP_GZ=1 to keep the download (deleted after a successful ingest by default).
# Only the 2018 to 2020 files publish an .md5sum; for the rest the gzip CRC checked while
# decompressing and the SHA-256 recorded by ingest are the integrity record.
set -euo pipefail

name=${1:?usage: ingest_day.sh <remote-file-name>}
base=${HFT_BASE:-"https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"}
data=${HFT_DATA:-$HOME/data}
ingest=${HFT_INGEST:-$(dirname "$0")/../build/release/apps/ingest}
checkpoint=${HFT_CHECKPOINT:-$(dirname "$0")/../build/release/apps/checkpoint}
gz="$data/$name"
store="$data/store-${name%%.*}"

mkdir -p "$data"
[ ! -e "$store" ] || { echo "store exists: $store" >&2; exit 1; }

want_size=$(curl -sfI --http1.1 "$base/$name" | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}' || true)
[ -n "$want_size" ] || { echo "not found on server: $name" >&2; exit 1; }

for i in $(seq 1 50); do
    have_size=$(stat -L -c %s "$gz" 2>/dev/null || echo 0)
    [ "$have_size" = "$want_size" ] && break
    curl -sS --http1.1 -C - -o "$gz" "$base/$name" || { echo "retry $i" >&2; sleep 5; }
done
[ "$(stat -L -c %s "$gz")" = "$want_size" ] || { echo "download incomplete" >&2; exit 1; }
if [ "$(head -c2 "$gz" | od -An -tx1 | tr -d ' \n')" != 1f8b ]; then
    rm -f "$gz"
    echo "not a gzip file (wrong name?): $name" >&2
    exit 1
fi

if md5_line=$(curl -sf --http1.1 "$base/$name.md5sum"); then
    [ "${md5_line%% *}" = "$(md5sum "$gz" | cut -d' ' -f1)" ] || { echo "md5 mismatch" >&2; exit 1; }
    echo "md5 ok"
fi

"$ingest" "$store" "$gz"
"$checkpoint" "$store"
[ "${KEEP_GZ:-0}" = 1 ] || rm "$gz"
