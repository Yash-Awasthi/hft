"""MS6 backtest report (DESIGN.md section 4) from the sweeps written by research/backtests.py.

Per strategy and tick group: PnL per stock-day with a bootstrap interval over stocks,
attribution (spread capture, inventory PnL split into adverse selection and residual, fees),
fills, volume, inventory, PnL per share, Sharpe on 5-minute buckets of the group's summed PnL
and the deflated Sharpe with the Q1 trial count from the registry. Then the latency curve,
fee and fill-rule sensitivity, ablations and sanity strategies.

Usage: python research/backtest_report.py --main val-main --latency val-latency
           --sensitivity val-sens --ablation val-ablation --sanity val-sanity
           [--hjb hjb.json] --out docs/results/ms6-backtest.md
"""

import argparse
import itertools
import json
import pathlib
import sqlite3
import sys

import numpy as np
import polars as pl

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research"))
import stats  # noqa: E402

DATA = pathlib.Path.home() / "data"
UNIT = 1e-6  # accounting unit in dollars (micro-dollars, src/backtest/accounting.hpp)
KEYS = ["strategy", "symbol", "day", "latency_us", "fees", "fill_rule"]
TERMS = ["spread", "adverse", "residual", "fees"]


def load(name):
    d = DATA / "runs" / name
    return pl.read_parquet(d / "summary.parquet"), pl.read_parquet(d / "minute_pnl.parquet")


def strategy_table(s, by=("strategy", "group"), n_boot=4000):
    """One row per group of `by`: means per stock-day in dollars and a bootstrap interval of
    the PnL mean resampling stocks (one cluster per symbol)."""
    s = s.with_columns(residual=pl.col("inventory_pnl") - pl.col("adverse"))
    rows = []
    for key, g in s.group_by(list(by), maintain_order=True):
        pnl = g["total"].to_numpy() * UNIT
        m, lo, hi = stats.cluster_bootstrap(pnl, g["symbol"].to_numpy(), n=n_boot)
        row = dict(zip(by, key))
        row.update(stock_days=len(g), pnl=m, lo=lo, hi=hi,
                   **{t: float(g[t].mean()) * UNIT for t in TERMS},
                   fills=float(g["fills"].mean()), volume=float(g["volume"].mean()),
                   max_inv_mean=float(g["max_abs_inventory"].mean()),
                   max_inv=int(g["max_abs_inventory"].max()),
                   pnl_per_share=float(g["total"].sum()) * UNIT / max(int(g["volume"].sum()), 1))
        rows.append(row)
    return pl.DataFrame(rows)


def bucket_returns(minutes, symbols, strategy, width=5, **config):
    """Summed PnL of `symbols` per `width`-minute bucket, in dollars, from cumulative minute PnL."""
    f = minutes.filter(pl.col("strategy") == strategy, pl.col("symbol").is_in(symbols))
    for k, v in config.items():
        f = f.filter(pl.col(k) == v)
    f = f.sort(["symbol", "day", "minute"]).with_columns(
        inc=pl.col("pnl").diff().over(["symbol", "day"]).fill_null(pl.col("pnl")))
    b = (f.with_columns(bucket=pl.col("minute") // width).group_by(["day", "bucket"])
         .agg(pl.col("inc").sum()).sort(["day", "bucket"]))
    return b["inc"].to_numpy() * UNIT


def trial_count(db_path, question):
    """Distinct (strategy, latency, fees, fill rule) configurations over every run of a question."""
    with sqlite3.connect(db_path) as db:
        rows = db.execute("SELECT config FROM runs WHERE question = ?", (question,)).fetchall()
    seen = set()
    for (cfg,) in rows:
        c = json.loads(cfg)
        seen.update(itertools.product(*(str(c[k]).split(",") for k in ("strategies", "latency_us", "fees", "fill_rule"))))
    return len(seen)


def latency_curve(s, strategy):
    return (s.filter(pl.col("strategy") == strategy).group_by(["group", "latency_us"])
            .agg(pnl=pl.col("total").mean() * UNIT, fills=pl.col("fills").mean(),
                 per_share=pl.col("total").sum() * UNIT / pl.col("volume").sum())
            .sort(["group", "latency_us"]))


def sharpe_table(s, minutes, by, n_trials):
    """5-minute Sharpe per group of `by` and the deflated Sharpe against n_trials."""
    groups = {g: s.filter(pl.col("group") == g)["symbol"].unique().to_list() for g in s["group"].unique()}
    rows = []
    for key, g in s.group_by(list(by), maintain_order=True):
        k = dict(zip(by, key))
        cfg = {c: k[c] for c in k if c in ("latency_us", "fees", "fill_rule")}
        r = bucket_returns(minutes, groups[k["group"]], k["strategy"], **cfg)
        rows.append({**k, "returns": r, "sharpe": stats.sharpe(r)})
    sr = np.array([r["sharpe"] for r in rows])
    var_sr = float(np.nanvar(sr, ddof=1)) if np.isfinite(sr).sum() > 1 else 0.0
    out = []
    for r in rows:
        dsr = stats.deflated_sharpe(r["returns"], n_trials, var_sr) if np.isfinite(r["sharpe"]) else float("nan")
        out.append({**{c: r[c] for c in by}, "sharpe": r["sharpe"], "dsr": dsr, "buckets": len(r["returns"])})
    return pl.DataFrame(out), var_sr


def fmt(x, nd=2):
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return "-"
    if isinstance(x, (int, np.integer)):
        return f"{x:,}"
    return f"{x:,.{nd}f}"


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(map(str, r)) + " |" for r in rows]
    return "\n".join(out)


def main_section(s, minutes, n_trials):
    t = strategy_table(s)
    sh, var_sr = sharpe_table(s, minutes, ("strategy", "group"), n_trials)
    t = t.join(sh, on=["strategy", "group"])
    rows = [[r["group"], r["strategy"], r["stock_days"], f"{fmt(r['pnl'])} [{fmt(r['lo'])}, {fmt(r['hi'])}]",
             fmt(r["spread"]), fmt(r["adverse"]), fmt(r["residual"]), fmt(r["fees"]), fmt(r["fills"], 0),
             fmt(r["volume"], 0), f"{fmt(r['max_inv_mean'], 0)} / {r['max_inv']}", fmt(r["pnl_per_share"] * 100, 3),
             fmt(r["sharpe"], 3), fmt(r["dsr"], 3)] for r in t.sort(["group", "strategy"]).iter_rows(named=True)]
    head = ["Group", "Strategy", "Stock-days", "PnL $ [95% CI]", "Spread $", "Adverse $", "Residual inv. $",
            "Fees $", "Fills", "Volume", "Max inv. mean / max", "PnL c/share", "Sharpe 5 min", "DSR"]
    return md_table(head, rows), var_sr


def config_table(s, by, label):
    t = strategy_table(s, by=by, n_boot=2000)
    rows = [[*(r[c] for c in by), f"{fmt(r['pnl'])} [{fmt(r['lo'])}, {fmt(r['hi'])}]", fmt(r["spread"]),
             fmt(r["adverse"]), fmt(r["fees"]), fmt(r["fills"], 0), fmt(r["pnl_per_share"] * 100, 3)]
            for r in t.sort(list(by)).iter_rows(named=True)]
    return md_table([*label, "PnL $ [95% CI]", "Spread $", "Adverse $", "Fees $", "Fills", "PnL c/share"], rows)


def ablation_section(s):
    t = strategy_table(s, n_boot=2000)
    ref = {r["group"]: r["pnl"] for r in t.filter(pl.col("strategy") == "ext").iter_rows(named=True)}
    order = ["ext", "ext_no_toxicity", "ext_no_taking", "dp_signal", "dp"]
    removed = {"ext": "-", "ext_no_toxicity": "toxicity guard", "ext_no_taking": "aggressive taking",
               "dp_signal": "both extensions", "dp": "extensions and signal"}
    rows = []
    for r in sorted(t.iter_rows(named=True), key=lambda r: (r["group"], order.index(r["strategy"]))):
        rows.append([r["group"], r["strategy"], removed[r["strategy"]],
                     f"{fmt(r['pnl'])} [{fmt(r['lo'])}, {fmt(r['hi'])}]", fmt(r["pnl"] - ref[r["group"]]),
                     fmt(r["fills"], 0), fmt(r["pnl_per_share"] * 100, 3)])
    return md_table(["Group", "Strategy", "Removed", "PnL $ [95% CI]", "Change vs ext $", "Fills", "PnL c/share"], rows)


def determinism(a, b):
    """Rows present in both sweeps with identical totals (same config twice must match)."""
    j = a.join(b, on=KEYS, suffix="_b")
    return len(j), int((j["total"] == j["total_b"]).sum())


def main():
    ap = argparse.ArgumentParser()
    for k in ("main", "latency", "sensitivity", "ablation", "sanity"):
        ap.add_argument(f"--{k}", required=True)
    ap.add_argument("--hjb")
    ap.add_argument("--latency-strategy", default="ext")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    n_trials = trial_count(DATA / "registry.sqlite", "Q1")
    s_main, m_main = load(a.main)
    s_lat, _ = load(a.latency)
    s_sens, _ = load(a.sensitivity)
    s_abl, _ = load(a.ablation)
    s_san, m_san = load(a.sanity)
    days = sorted(s_main["day"].unique().to_list())
    main_tab, var_sr = main_section(s_main, m_main, n_trials)
    lat = latency_curve(s_lat, a.latency_strategy)
    det_n, det_eq = determinism(s_main, s_abl)

    out = [
        "# MS6: quoting and backtest",
        "",
        f"Validation day{'s' if len(days) > 1 else ''} {', '.join(days)} only; the test days stay locked. "
        f"{s_main['symbol'].n_unique()} universe stocks (25 large-tick, 25 small-tick). Dollars per stock-day. "
        "With a single validation day the 95% intervals resample stocks, not days: they measure "
        "dispersion across stocks on one day and say nothing about day-to-day variation.",
        "",
        "Attribution: total = spread capture + inventory PnL + fees, checked after every event in "
        "the backtest. Inventory PnL is split into adverse selection (mid move over the 1 s after each "
        "of our fills, times the fill) and the residual. Fees include rebates, access fees, Section 31 "
        "and FINRA TAF (`configs/fees.toml`).",
        "",
        f"Sharpe: mean over standard deviation of the group's summed PnL per 5-minute bucket "
        f"(about 78 buckets per day; not annualized). Deflated Sharpe (Bailey and Lopez de Prado): "
        f"probability that the true Sharpe exceeds the expected maximum of {n_trials} unskilled "
        f"trials (distinct Q1 configurations in the registry), Sharpe variance across trials "
        f"{var_sr:.4f}.",
        "",
        "## Strategies (latency 0, 30 mil cap, queue fill rule)",
        "",
        main_tab,
        "",
        "## Sanity strategies",
        "",
        config_table(s_san, ("group", "strategy"), ["Group", "Strategy"]),
        "",
        f"## Latency ({a.latency_strategy})",
        "",
        "Market-data and order-entry latency both set to the value; processing 5 us.",
        "",
        md_table(["Group", "Latency us", "PnL $", "Fills", "PnL c/share"],
                 [[r["group"], fmt(r["latency_us"], 0), fmt(r["pnl"]), fmt(r["fills"], 0),
                   fmt(r["per_share"] * 100, 3)] for r in lat.iter_rows(named=True)]),
        "",
        "## Fee schedule and fill rule",
        "",
        config_table(s_sens, ("group", "strategy", "fees", "fill_rule"), ["Group", "Strategy", "Fees", "Fill rule"]),
        "",
        "## Ablation",
        "",
        ablation_section(s_abl),
        "",
        f"Determinism: {det_eq} of {det_n} jobs repeated across the main and ablation sweeps give identical totals.",
        "",
    ]
    if a.hjb:
        h = json.loads(pathlib.Path(a.hjb).read_text())
        out += ["## Avellaneda-Stoikov closed forms against the numerical HJB", "", h["text"], ""]
    pathlib.Path(a.out).write_text("\n".join(out))
    print(f"wrote {a.out}, trials {n_trials}")


if __name__ == "__main__":
    main()
