"""Restores the fee schedule label of sweeps written before backtests.py kept it apart from
the fee PnL (both were called `fees`, and the PnL overwrote the label). Each job's fee PnL is
recomputed from its fills with the backtest's integer arithmetic (src/backtest/backtest.hpp)
under every schedule of the sweep; the label is the schedule that reproduces it exactly. Jobs
without fills are labelled by elimination within their (strategy, symbol, latency, fill rule).

Usage: python research/repair_fee_labels.py <sweep-name>...
"""

import json
import pathlib
import sqlite3
import sys
import tomllib

import numpy as np
import polars as pl

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA = pathlib.Path.home() / "data"
GROUP = ["strategy", "symbol", "day", "latency_us", "fill_rule"]


def fee_pnl(price, shares, maker, sched, reg):
    total = 0
    for p, s, m in zip(price.tolist(), shares.tolist(), maker.tolist()):
        q = abs(int(s))
        fee = -sched["maker_rebate"] * q if m else sched["taker_fee"] * q
        if s < 0:
            notional = int(p) * 100 * q
            fee += notional // 1_000_000 * reg["sec_fee_per_million"] // 1_000_000
            fee += min(reg["taf_max"], reg["taf_per_share"] * q)
        total -= fee
    return total


def repair(name):
    out = DATA / "runs" / name
    s = pl.read_parquet(out / "summary.parquet")
    if "fee_schedule" in s.columns:
        return f"{name}: already labelled"
    with sqlite3.connect(DATA / "registry.sqlite") as db:
        cfg = json.loads(db.execute("SELECT config FROM runs WHERE name = ? ORDER BY run_id DESC", (name,)).fetchone()[0])
    fees = tomllib.loads((ROOT / "configs/fees.toml").read_text())
    scheds = cfg["fees"].split(",")
    fills = pl.read_parquet(out / "fills.parquet") if (out / "fills.parquet").exists() else None
    labels, checked = [], 0
    for r in s.iter_rows(named=True):
        f = (fills.filter(*[pl.col(k) == r[k] for k in GROUP], pl.col("fees") == r["fees"])
             if fills is not None else None)
        if f is None or f.height == 0:
            labels.append(None)
            continue
        hit = [sc for sc in scheds if fee_pnl(f["price"].to_numpy(), f["shares"].to_numpy(), f["maker"].to_numpy(),
                                               fees[sc], fees["regulatory"]) == r["fees"]]
        if len(hit) != 1:
            raise SystemExit(f"{name}: {len(hit)} schedules reproduce {r['strategy']} {r['symbol']} {r['fees']}")
        labels.append(hit[0])
        checked += 1
    s = s.with_columns(fee_schedule=pl.Series(labels, dtype=pl.Utf8))
    # Jobs without fills: the schedules not taken by their group's labelled jobs.
    filled = []
    for _, g in s.with_row_index().group_by(GROUP, maintain_order=True):
        free = list(scheds)
        for lab in g["fee_schedule"].to_list():
            if lab is not None:
                free.remove(lab)
        filled += [(i, free.pop()) for i, lab in zip(g["index"].to_list(), g["fee_schedule"].to_list()) if lab is None]
    lab = s["fee_schedule"].to_list()
    for i, v in filled:
        lab[i] = v
    s = s.with_columns(fee_schedule=pl.Series(lab))
    # Minute and fill rows were written job by job in summary order: label them by position,
    # checking every block's keys against its job.
    lab = s["fee_schedule"].to_list()
    m = pl.read_parquet(out / "minute_pnl.parquet")
    per = m.height // s.height
    assert per * s.height == m.height
    chk = m.select(*GROUP, "fees").gather(np.arange(s.height) * per)
    assert chk.equals(s.select(*GROUP, "fees")), "minute blocks out of order"
    m.drop("fees").with_columns(fee_schedule=pl.Series(np.repeat(lab, per))).write_parquet(out / "minute_pnl.parquet")
    if fills is not None:
        n = s["fills"].to_numpy()
        assert n.sum() == fills.height
        starts = np.r_[0, np.cumsum(n)[:-1]][n > 0]
        chk = fills.select(*GROUP, "fees").gather(starts)
        assert chk.equals(s.filter(pl.col("fills") > 0).select(*GROUP, "fees")), "fill blocks out of order"
        fills.drop("fees").with_columns(fee_schedule=pl.Series(np.repeat(lab, n))).write_parquet(out / "fills.parquet")
    s.write_parquet(out / "summary.parquet")
    return f"{name}: {checked} jobs reproduced exactly, {len(filled)} by elimination, schedules {scheds}"


if __name__ == "__main__":
    for n in sys.argv[1:]:
        print(repair(n))
