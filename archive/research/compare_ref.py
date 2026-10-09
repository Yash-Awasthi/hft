"""Compares research/ref_book.py with a book_replay output on a seeded sample of symbols.

Usage: python research/compare_ref.py <store-dir> <book_replay-output> [n] [seed]
Exits non-zero on any mismatch in BBO change count, hash, errors or executed volume.
"""

import random
import subprocess
import sys
from pathlib import Path


def main():
    store, table = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    rows = {}
    with open(table) as f:
        cols = f.readline().split()
        for line in f:
            r = dict(zip(cols, line.split()))
            if r["locate"].startswith("rc="):
                continue
            rows[int(r["locate"])] = r
    # Sample among symbols with book activity, plus the busiest one.
    active = sorted(k for k, r in rows.items() if int(r["book_msgs"]) > 0)
    sample = random.Random(seed).sample(active, min(n, len(active)))
    sample.append(max(active, key=lambda k: int(rows[k]["msgs"])))
    ref = Path(__file__).with_name("ref_book.py")
    bad = 0
    for loc in sorted(set(sample)):
        out = subprocess.run([sys.executable, str(ref), store, str(loc)], check=True,
                             capture_output=True, text=True).stdout.split()
        got = dict(kv.split("=") for kv in out[1:])
        r = rows[loc]
        same = (got["changes"] == r["bbo_changes"] and got["bbo_hash"] == r["bbo_hash"]
                and got["errors"] == r["errors"] and got["executed"] == r["executed"])
        bad += not same
        print(f"{loc} {r['symbol']} msgs={r['msgs']} changes={got['changes']} "
              f"hash={got['bbo_hash']} {'ok' if same else 'MISMATCH'}")
    print(f"{len(set(sample)) - bad} of {len(set(sample))} symbols agree")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
