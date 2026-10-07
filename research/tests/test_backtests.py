"""Sweep rows keep the fee schedule label next to the fee PnL the backtest returns."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import backtests  # noqa: E402


def test_summary_row_keeps_the_fee_schedule_and_the_fee_pnl():
    job = {"strategy": "naive", "group": "large_tick", "symbol": "X", "day": "2025-12-11",
           "latency_us": 0.0, "fees": "cap10", "fill_rule": "queue"}
    r = {"total": 5, "fees": -7, "minute_pnl": np.zeros(3)}
    row = backtests.summary_row(job, r)
    assert row["fee_schedule"] == "cap10" and row["fees"] == -7 and row["total"] == 5
    assert "minute_pnl" not in row


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
