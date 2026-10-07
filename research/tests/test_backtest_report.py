"""Backtest report checks: attribution terms add up to the total, bucket returns from the
cumulative minute PnL, Q1 trial count from the registry, and the latency and ablation tables."""

import json
import pathlib
import sqlite3
import sys
import tempfile

import numpy as np
import polars as pl

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import backtest_report as br  # noqa: E402


def summary(rows):
    base = {"day": "2025-12-11", "latency_us": 0.0, "fee_schedule": "cap30", "fill_rule": "queue", "fills": 10,
            "volume": 1000, "max_abs_inventory": 200, "orders": 20}
    return pl.DataFrame([{**base, **r} for r in rows])


def test_attribution_splits_inventory_into_adverse_and_residual():
    s = summary([
        {"strategy": "a", "group": "g", "symbol": "X", "total": 100, "spread": 300, "inventory_pnl": -150,
         "adverse": -220, "fees": -50},
        {"strategy": "a", "group": "g", "symbol": "Y", "total": 60, "spread": 100, "inventory_pnl": -20,
         "adverse": -10, "fees": -20},
    ])
    t = br.strategy_table(s, n_boot=200)
    row = t.row(0, named=True)
    u = br.UNIT
    assert np.isclose(row["pnl"], 80 * u)
    assert np.isclose(row["spread"], 200 * u) and np.isclose(row["adverse"], -115 * u)
    assert np.isclose(row["residual"], (-85 + 115) * u) and np.isclose(row["fees"], -35 * u)
    assert np.isclose(row["spread"] + row["adverse"] + row["residual"] + row["fees"], row["pnl"])
    assert row["lo"] <= row["pnl"] <= row["hi"]
    assert np.isclose(row["pnl_per_share"], 160 * u / 2000)
    assert row["stock_days"] == 2


def test_bucket_returns_sum_symbols_and_difference_cumulative_pnl():
    mins = []
    for sym, inc in (("X", 1), ("Y", 2)):
        cum = np.cumsum(np.full(10, inc))
        mins.append(pl.DataFrame({"strategy": ["a"] * 10, "symbol": [sym] * 10, "day": ["d"] * 10,
                                  "latency_us": [0.0] * 10, "fee_schedule": ["cap30"] * 10, "fill_rule": ["queue"] * 10,
                                  "minute": list(range(10)), "pnl": cum.tolist()}))
    r = br.bucket_returns(pl.concat(mins), ["X", "Y"], "a", width=5)
    assert np.allclose(r, [15 * br.UNIT, 15 * br.UNIT])


def test_trial_count_counts_distinct_configurations_across_runs():
    with tempfile.TemporaryDirectory() as d:
        db = sqlite3.connect(pathlib.Path(d) / "r.sqlite")
        db.execute("CREATE TABLE runs (name TEXT, question TEXT, config TEXT)")
        for name, strats, lat in (("a", "x,y", "0"), ("b", "x", "0,10"), ("c", "z", "0")):
            q = "Q2" if name == "c" else "Q1"
            db.execute("INSERT INTO runs VALUES (?, ?, ?)", (name, q, json.dumps(
                {"strategies": strats, "latency_us": lat, "fees": "cap30", "fill_rule": "queue"})))
        db.commit()
        assert br.trial_count(pathlib.Path(d) / "r.sqlite", "Q1") == 3  # x@0, y@0, x@10


def test_latency_curve_is_mean_per_group_and_latency():
    s = summary([
        {"strategy": "e", "group": "g", "symbol": "X", "latency_us": 0.0, "total": 100},
        {"strategy": "e", "group": "g", "symbol": "Y", "latency_us": 0.0, "total": 300},
        {"strategy": "e", "group": "g", "symbol": "X", "latency_us": 50.0, "total": -100},
    ])
    c = br.latency_curve(s, "e").sort("latency_us")
    assert np.allclose(c["pnl"].to_list(), [200 * br.UNIT, -100 * br.UNIT])


def test_sensitivity_is_optional_and_the_report_says_so():
    a = br.parser().parse_args(["--main", "m", "--latency", "l", "--ablation", "b", "--sanity", "s", "--out", "o"])
    assert a.sensitivity is None
    text = "\n".join(br.sensitivity_section(None))
    assert "## Fee schedule and fill rule" in text and "not included" in text


def test_idle_lists_configurations_that_sent_no_order_with_their_rejects():
    s = summary([
        {"strategy": "a", "group": "g", "symbol": "X", "orders": 0, "rejects": 10},
        {"strategy": "a", "group": "g", "symbol": "Y", "orders": 0, "rejects": 30},
        {"strategy": "b", "group": "g", "symbol": "X", "orders": 5, "rejects": 0},
    ])
    assert br.idle(s) == [("g", "a", 20.0)]


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
