"""Field-level comparison of the ITCH decoder against the third-party itchfeed parser.

itchfeed (github.com/bbalouki/itch, MIT) is not a project dependency: unpack its wheel into
a directory and pass that directory. Its pure-Python backend is forced.

Usage: python -I research/compare_itchfeed.py <itchfeed-dir> <slice.bin> <itch_dump-output>
"""

import collections
import os
import sys

FIELDS = {
    "S": ["event_code"],
    "R": ["stock", "market_category", "financial_status_indicator", "round_lot_size",
          "round_lots_only", "issue_classification", "issue_sub_type", "authenticity",
          "short_sale_threshold_indicator", "ipo_flag", "luld_ref", "etp_flag",
          "etp_leverage_factor", "inverse_indicator"],
    "H": ["stock", "trading_state", "reserved", "reason"],
    "A": ["order_reference_number", "buy_sell_indicator", "shares", "stock", "price"],
    "F": ["order_reference_number", "buy_sell_indicator", "shares", "stock", "price",
          "attribution"],
    "E": ["order_reference_number", "executed_shares", "match_number"],
    "C": ["order_reference_number", "executed_shares", "match_number", "printable",
          "execution_price"],
    "X": ["order_reference_number", "cancelled_shares"],
    "D": ["order_reference_number"],
    "U": ["order_reference_number", "new_order_reference_number", "shares", "price"],
    "P": ["order_reference_number", "buy_sell_indicator", "shares", "stock", "price",
          "match_number"],
    "Q": ["shares", "stock", "cross_price", "match_number", "cross_type"],
    "B": ["match_number"],
    "I": ["paired_shares", "imbalance_shares", "imbalance_direction", "stock", "far_price",
          "near_price", "current_reference_price", "cross_type", "variation_indicator"],
}


def canonical(m):
    t = m.message_type.decode()
    parts = [t, str(m.stock_locate), str(m.tracking_number), str(m.timestamp)]
    for f in FIELDS.get(t, []):
        v = getattr(m, f)
        parts.append(v.decode("latin-1") if isinstance(v, bytes) else str(v))
    return "|".join(parts)


def main():
    pkg, slice_path, dump_path = sys.argv[1:4]
    os.environ["ITCH_NO_CPP"] = "1"
    sys.path.insert(0, pkg)
    from itch.parser import MessageParser

    with open(dump_path, encoding="latin-1") as f:
        ours = [line.rstrip("\n") for line in f]
    counts = collections.Counter()
    mismatches = 0
    n = 0
    with open(slice_path, "rb") as f:
        for i, m in enumerate(MessageParser().parse_file(f)):
            theirs = canonical(m)
            counts[theirs[0]] += 1
            if i >= len(ours) or ours[i] != theirs:
                mismatches += 1
                if mismatches <= 5:
                    print(f"mismatch at {i}:\n  ours   {ours[i] if i < len(ours) else None}\n"
                          f"  theirs {theirs}")
            n += 1
    if n != len(ours):
        mismatches += 1
        print(f"message count differs: ours {len(ours)}, theirs {n}")
    print(" ".join(f"{t}={c}" for t, c in sorted(counts.items())))
    print(f"{n} messages, {mismatches} mismatches")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
