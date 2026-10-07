"""Fee PnL recomputed from fills with the backtest's integer arithmetic."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import repair_fee_labels as rf  # noqa: E402

REG = {"sec_fee_per_million": 27_800_000, "taf_per_share": 166, "taf_max": 8_300_000}


def test_fee_pnl_matches_hand_computation():
    # maker buy 100 @ $10.00, taker sell 200 @ $10.01
    price = np.array([100_000, 100_100], dtype=np.uint32)
    shares = np.array([100, -200])
    maker = np.array([1, 0], dtype=np.uint8)
    sched = {"maker_rebate": 2000, "taker_fee": 3000}
    notional = 100_100 * 100 * 200
    sell_fee = 3000 * 200 + notional // 1_000_000 * 27_800_000 // 1_000_000 + 166 * 200
    assert rf.fee_pnl(price, shares, maker, sched, REG) == 2000 * 100 - sell_fee


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
