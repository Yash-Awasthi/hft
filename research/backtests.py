"""Backtest sweeps over strategies, symbols, days, latencies, fee scenarios and fill rules
(DESIGN.md sections 4 and 8). Jobs run in a process pool; each writes one summary row, its
minute PnL and its fills. The sweep is logged in the registry with its trial count.

Usage: python research/backtests.py <py-build> --name <sweep> --days <YYYY-MM-DD>...
           [--strategies naive,as,dp,dp_signal] [--latency-us 0] [--fees cap30]
           [--fill-rule queue] [--symbols ...] [--workers 6] [--allow-test]
"""

import argparse
import hashlib
import json
import multiprocessing as mp
import pathlib
import sqlite3
import subprocess
import sys
import time
import tomllib

import polars as pl

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA = pathlib.Path.home() / "data"
POLICIES = DATA / "policies"


def strategy_spec(name, group):
    meta = POLICIES / f"dp_{group}_nosignal.bin.json"
    as_A, as_k, vol = 1.0, 1.5, 1.0
    if meta.exists():
        m = json.loads(meta.read_text())
        as_A, as_k, vol = m["as_A"], m["as_k"], m.get("vol_p95", 1.0)
    ext = f"ext:{POLICIES / f'dp_{group}_signal.bin'}"
    specs = {
        "zero": ("zero", {}),
        "naive": ("naive", {"size": 100, "max_inventory": 500}),
        "random_taker": ("random_taker", {"rate": 0.001}),
        "random_passive": ("random_passive", {}),
        "foresight": ("perfect_foresight", {"horizon_ns": 1e9, "cost_ticks": 0.3}),
        "as": ("avellaneda_stoikov", {"gamma": 0.05, "A": as_A, "k": as_k, "glft": 1,
                                      "size": 100, "max_inventory": 500, "hysteresis_ticks": 1}),
        "as_online": ("avellaneda_stoikov", {"gamma": 0.05, "A": as_A, "k": as_k, "glft": 1, "online": 1,
                                             "size": 100, "max_inventory": 500, "hysteresis_ticks": 1}),
        "dp": (f"dp:{POLICIES / f'dp_{group}_nosignal.bin'}", {}),
        "dp_signal": (f"dp:{POLICIES / f'dp_{group}_signal.bin'}", {}),
        "ext": (ext, {"vol_limit": vol, "toxicity": 1, "taking": 1}),
        "ext_no_toxicity": (ext, {"vol_limit": vol, "toxicity": 0, "taking": 1}),
        "ext_no_taking": (ext, {"vol_limit": vol, "toxicity": 1, "taking": 0}),
    }
    return specs[name]


def summary_row(job, r):
    """One summary row: the job's labels (the fee scenario as fee_schedule, since the backtest's
    own `fees` field is the fee PnL) and the backtest's scalar results."""
    row = {k: job[k] for k in ("strategy", "group", "symbol", "day", "latency_us", "fill_rule")}
    row["fee_schedule"] = job["fees"]
    row.update({k: int(v) for k, v in r.items() if not hasattr(v, "shape")})
    return row


def run_job(job):
    sys.path.insert(0, job["py_build"])
    import hftpy

    t0 = time.time()
    name, params = strategy_spec(job["strategy"], job["group"])
    r = hftpy.backtest(job["store"], job["locate"], job["index"], name, params, job["config"])
    row = summary_row(job, r)
    row["seconds"] = time.time() - t0
    minutes = r["minute_pnl"].tolist()
    fills = {"ts": r["fill_ts"].tolist(), "shares": r["fill_signed_shares"].tolist(),
             "price": r["fill_price"].tolist(), "maker": r["fill_maker"].tolist()}
    return row, minutes, fills


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("--name", required=True)
    ap.add_argument("--days", nargs="+", required=True)
    ap.add_argument("--strategies", default="naive,as,dp,dp_signal")
    ap.add_argument("--latency-us", default="0")
    ap.add_argument("--fees", default="cap30")
    ap.add_argument("--fill-rule", default="queue")
    ap.add_argument("--symbols", nargs="*")
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--allow-test", action="store_true")
    ap.add_argument("--question", default="Q1")
    a = ap.parse_args()

    splits = tomllib.loads((ROOT / "configs/splits.toml").read_text())
    test_days = set(splits["test"]["days"])
    if test_days & set(a.days) and not a.allow_test:
        sys.exit("refusing locked test days without --allow-test")
    fees = tomllib.loads((ROOT / "configs/fees.toml").read_text())
    universe = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    groups = {s: g for g in ("large_tick", "small_tick") for s in universe[g]["symbols"]}
    symbols = a.symbols or list(groups)

    sys.path.insert(0, a.py_build)
    import hftpy

    jobs = []
    for day in a.days:
        store = str(DATA / f"store-{splits['files'][day].split('.')[0]}")
        sym = hftpy.symbols(store)
        for s in symbols:
            if s not in sym:
                continue
            for strat in a.strategies.split(","):
                for lat in map(float, a.latency_us.split(",")):
                    for fee in a.fees.split(","):
                        for rule in a.fill_rule.split(","):
                            cfg = {"tick": 100, "maker_rebate": fees[fee]["maker_rebate"],
                                   "taker_fee": fees[fee]["taker_fee"],
                                   "fill_rule": 1 if rule == "trade_through" else 0,
                                   "market_data_ns": lat * 1000, "order_entry_ns": lat * 1000,
                                   "processing_ns": 5000, **fees["regulatory"]}
                            jobs.append({"py_build": a.py_build, "store": store, "locate": sym[s],
                                         "index": [sym["SPY"], sym["QQQ"]], "symbol": s,
                                         "group": groups[s], "day": day, "strategy": strat,
                                         "latency_us": lat, "fees": fee, "fill_rule": rule, "config": cfg})
    out = DATA / "runs" / a.name
    out.mkdir(parents=True, exist_ok=True)
    spec = {"name": a.name, "days": a.days, "strategies": a.strategies, "latency_us": a.latency_us,
            "fees": a.fees, "fill_rule": a.fill_rule, "symbols": symbols, "fee_table": fees}
    canonical = json.dumps(spec, sort_keys=True)
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    data_hash = hashlib.sha256("".join((pathlib.Path(j["store"]) / "source.sha256").read_text()
                                       for j in {j["store"]: j for j in jobs}.values()).encode()).hexdigest()

    t0 = time.time()
    rows, minutes, fills = [], [], []
    with mp.get_context("spawn").Pool(a.workers) as pool:
        for i, (row, mins, fl) in enumerate(pool.imap_unordered(run_job, jobs)):
            rows.append(row)
            key = {k: row[k] for k in ("strategy", "symbol", "day", "latency_us", "fee_schedule", "fill_rule")}
            minutes.append(pl.DataFrame({**{k: [v] * len(mins) for k, v in key.items()},
                                         "minute": list(range(len(mins))), "pnl": mins}))
            if fl["ts"]:
                fills.append(pl.DataFrame({**{k: [v] * len(fl["ts"]) for k, v in key.items()}, **fl}))
            if (i + 1) % 20 == 0:
                print(f"{i + 1}/{len(jobs)} jobs, {time.time() - t0:.0f}s", flush=True)
    summary = pl.DataFrame(rows)
    summary.write_parquet(out / "summary.parquet")
    pl.concat(minutes).write_parquet(out / "minute_pnl.parquet")
    if fills:
        pl.concat(fills).write_parquet(out / "fills.parquet")
    with sqlite3.connect(DATA / "registry.sqlite") as db:
        db.execute("""CREATE TABLE IF NOT EXISTS runs (run_id INTEGER PRIMARY KEY,
            started TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), finished TEXT,
            name TEXT NOT NULL, question TEXT NOT NULL, config_hash TEXT NOT NULL, config TEXT NOT NULL,
            commit_hash TEXT NOT NULL, data_hash TEXT NOT NULL, test_access INTEGER NOT NULL,
            metrics TEXT, outputs TEXT)""")
        trials = len({(j["strategy"], j["latency_us"], j["fees"], j["fill_rule"]) for j in jobs})
        db.execute("INSERT INTO runs (finished, name, question, config_hash, config, commit_hash, data_hash, "
                   "test_access, metrics, outputs) VALUES (strftime('%Y-%m-%dT%H:%M:%fZ','now'), ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                   (a.name, a.question, hashlib.sha256(canonical.encode()).hexdigest(), canonical, commit,
                    data_hash, int(bool(test_days & set(a.days))),
                    json.dumps({"jobs": len(jobs), "trials": trials}), str(out)))
    print(f"{len(jobs)} jobs in {time.time() - t0:.0f}s -> {out}")


if __name__ == "__main__":
    main()
