"""Regime forecast method C (DESIGN.md section 5): the queue-reactive model calibrated per
stock on the train days, simulated at the current tick and at the half-penny tick (insertions
split evenly, or two thirds to the inner half-tick level, as the redistribution sensitivity),
each simulated session measured like real data: spread, depth at the best, queue turnover,
fill probability within 10 s by queue position at arrival, 1 s markouts of fills, and the
naive strategy's PnL per share in the backtest (30 mil fees now, 10 mil cap at the half tick).
The forecast for a stock is its observed value times the simulated ratio half / now.

Not modelled: the power-law feedback kernel of Noble et al. (the simulator is model I alone),
re-solving the DP policy at the new tick, and the queue value term for lower rebates.

Usage: python research/method_c.py <py-build> <ingest> <day_stats> --stores <train>... --out <dir>
           [--symbols ...] [--workers 3]
"""

import argparse
import json
import multiprocessing as mp
import pathlib
import subprocess
import sys
import tempfile
import tomllib

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research"))
import qr  # noqa: E402

K, N = 3, 50
OPEN, CLOSE = 34_200_000_000_000, 57_600_000_000_000
QUEUE_EDGES = [0, 1, 5, 20, np.inf]  # queue ahead at arrival, in AES


def measure(hftpy, tools, store, tick, aes, fees):
    rows = subprocess.run([tools["day_stats"], str(store)], capture_output=True, text=True, check=True).stdout
    head, *body = [r.split("\t") for r in rows.strip().split("\n")]
    row = dict(zip(head, next(b for b in body if b[1] == "QRSIM")))
    out = {"spread": float(row["spread_ticks"]) * 0.01, "depth": float(row["depth"]),
           "turnover": float(row["displayed"]) / 23_400 / max(float(row["depth"]), 1e-9),
           "eta": float(row["eta"])}
    loc = hftpy.symbols(str(store))["QRSIM"]
    lc = hftpy.lifecycles(str(store), loc, 5, 60.0, [1.0])
    ahead = lc["covariates"][:, 0] / aes
    filled10 = (lc["outcome"] == 1) & (lc["duration_s"] <= 10)
    out["fill10_by_queue"] = [float(filled10[(ahead >= lo) & (ahead < hi)].mean()) if ((ahead >= lo) & (ahead < hi)).sum() > 20 else None
                              for lo, hi in zip(QUEUE_EDGES[:-1], QUEUE_EDGES[1:])]
    mk = lc["fills"]["markout"][:, 0]
    out["markout_1s_cents"] = float(np.nanmean(mk)) if len(mk) else None
    cfg = {"tick": tick, "maker_rebate": fees["maker_rebate"], "taker_fee": fees["taker_fee"],
           "processing_ns": 5000, "start_ns": OPEN + 300e9, "stop_ns": CLOSE - 300e9, "end_ns": CLOSE - 60e9}
    bt = hftpy.backtest(str(store), loc, [], "naive", {"size": 100, "max_inventory": 500}, cfg)
    out["naive_pnl_per_share_cents"] = float(bt["total"]) * 1e-6 / max(int(bt["volume"]), 1) * 100
    out["naive_volume"] = int(bt["volume"])
    return out


def run_stock(job):
    sys.path.insert(0, job["py_build"])
    import hftpy

    fees = tomllib.loads((ROOT / "configs/fees.toml").read_text())
    fits, aes = [], None
    for store in job["stores"]:
        ev = hftpy.qr_events(store, hftpy.symbols(store)[job["symbol"]], K, 100, OPEN + 300_000_000_000,
                             CLOSE - 300_000_000_000)
        f = qr.calibrate(ev, K, N, tick=100, aes=aes)
        aes = f["aes"]
        fits.append(f)
        del ev
    fit = qr.pool(fits)
    p_ref = int(job["mid"] * 100) * 100 + 50
    res = {"symbol": job["symbol"], "aes": aes, "theta": fit["theta"], "theta_se": fit["theta_se"],
           "L1": fit["L"][0, :6].tolist(), "M1": fit["M"][0, :6].tolist()}
    scen = {"now": (fit, fees["cap30"]), "half_even": (qr.half_tick(fit, 0.5), fees["cap10"]),
            "half_inner": (qr.half_tick(fit, 2 / 3), fees["cap10"])}
    with tempfile.TemporaryDirectory(dir=job["tmp"]) as d:
        for name, (f, fee) in scen.items():
            raw, store = pathlib.Path(d) / f"{name}.itch", pathlib.Path(d) / name
            pr = p_ref if f["tick"] == 100 else int(job["mid"] * 200) * 50 + 25
            info = hftpy.qr_simulate(**qr.simulate_args(f, pr, OPEN, CLOSE, 7, raw))
            store.mkdir()
            with open(raw, "rb") as fh:
                subprocess.run([job["ingest"], str(store)], stdin=fh, check=True, capture_output=True)
            raw.unlink()
            res[name] = {"events": info["events"], "moves": info["moves"],
                         **measure(hftpy, job, store, f["tick"], aes, fee)}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("ingest")
    ap.add_argument("day_stats")
    ap.add_argument("--stores", nargs="+", required=True)
    ap.add_argument("--forecast", required=True, help="research/regime.py forecast parquet")
    ap.add_argument("--symbols", nargs="*")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    import polars as pl

    fc = pl.read_parquet(a.forecast)
    rows = fc.filter(pl.col("treated"))
    if a.symbols:
        rows = rows.filter(pl.col("symbol").is_in(a.symbols))
    mid = dict(zip(fc["symbol"], fc["mid"]))
    tmp = pathlib.Path(a.out) / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    jobs = [{"py_build": a.py_build, "ingest": a.ingest, "day_stats": a.day_stats, "stores": a.stores,
             "symbol": s, "mid": mid[s], "tmp": str(tmp)} for s in rows["symbol"]]
    out = []
    with mp.get_context("spawn").Pool(a.workers) as pool:
        for r in pool.imap_unordered(run_stock, jobs):
            out.append(r)
            print(r["symbol"], json.dumps({k: r[k] for k in ("now", "half_even")})[:300], flush=True)
    (pathlib.Path(a.out) / "method_c.json").write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
