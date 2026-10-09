"""MS7 regime forecast report and pre-registration bundle (DESIGN.md section 5).

Combines methods A (cross-sectional), B (implicit spread) and C (queue-reactive simulation) per
treated universe stock for the half-penny tick and 10 mil cap: weights from each method's error
predicting the validation day at the current tick (inverse squared median absolute log error),
bands spanning every method. Writes docs/results/ms7-regime.md and the bundle under
docs/prereg/: forecast.csv, scoring rules, and a manifest of input and code hashes.

Usage: python research/regime_report.py --regime <dir> --val-stats <val.tsv> --fit-stats <train.tsv>...
           --out-md <md> --bundle <dir>
"""

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tomllib

import numpy as np
import polars as pl

import regime

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def fmt(x, nd=3):
    return "-" if x is None or (isinstance(x, float) and not np.isfinite(x)) else f"{x:.{nd}f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--regime", required=True)
    ap.add_argument("--val-stats", required=True)
    ap.add_argument("--fit-stats", nargs="+", required=True)
    ap.add_argument("--out-md", required=True)
    ap.add_argument("--bundle", required=True)
    a = ap.parse_args()
    d = pathlib.Path(a.regime)
    rep = json.loads((d / "regime.json").read_text())
    fc = pl.read_parquet(d / "forecast_half_tick.parquet")
    mc = {r["symbol"]: r for r in json.loads((d / "method_c.json").read_text())}
    val = regime.load([a.val_stats])
    vmap = {r["symbol"]: r for r in val.iter_rows(named=True)}
    past = tomllib.loads((ROOT / "configs/past_tick_changes.toml").read_text())

    t = fc.filter(pl.col("treated") & pl.col("symbol").is_in(list(mc)))
    # Held-out errors at the current tick on the validation day (median absolute log error).
    err = {"spread": {"a": [], "b": [], "c": []}, "depth": {"a": [], "c": []}}
    for r in t.iter_rows(named=True):
        v = vmap.get(r["symbol"])
        if not v:
            continue
        c = mc[r["symbol"]]["now"]
        err["spread"]["b"].append(abs(np.log(max(0.01, 2 * r["eta"] * 0.01) / v["spread"])))
        err["spread"]["c"].append(abs(np.log(c["spread"] / v["spread"])))
        err["depth"]["c"].append(abs(np.log(c["depth"] / v["depth"])))
    # Method A: train-day fit scored on the validation day for the same stocks.
    train = regime.load(a.fit_stats)
    rows_val = val.filter(pl.col("symbol").is_in(t["symbol"].to_list()))
    for target, key in (("ln_spread", "spread"), ("ln_depth", "depth")):
        pred = regime.predict_a(regime.fit_a(train, target), rows_val)
        err[key]["a"] = list(np.abs(pred - rows_val[target].to_numpy()))
    errors = {k: {m: (float(np.median(v)) if v else None) for m, v in e.items()} for k, e in err.items()}
    w_spread, w_depth = regime.weights(errors["spread"]), regime.weights(errors["depth"])

    rows = []
    for r in t.sort("symbol").iter_rows(named=True):
        c = mc[r["symbol"]]
        cs = {"a": r["a_ln_spread_change"], "b": float(np.log(r["b_spread_beta1.0"] / r["spread_now"])),
              "c": float(np.log(c["half_even"]["spread"] / c["now"]["spread"]))}
        bands_s = {"a": (r["a_ln_spread_lo"], r["a_ln_spread_hi"]),
                   "b": tuple(sorted([float(np.log(r[f"b_spread_beta{b}"] / r["spread_now"])) for b in (0.5, 1.0)])),
                   "c": tuple(sorted([cs["c"], float(np.log(c["half_inner"]["spread"] / c["now"]["spread"]))]))}
        sp, sp_lo, sp_hi, sp_dis = regime.combine(cs, w_spread, bands_s)
        cd = {"a": r["a_ln_depth_change"], "c": float(np.log(c["half_even"]["depth"] / c["now"]["depth"]))}
        bands_d = {"a": (r["a_ln_depth_lo"], r["a_ln_depth_hi"]),
                   "c": tuple(sorted([cd["c"], float(np.log(c["half_inner"]["depth"] / c["now"]["depth"]))]))}
        dp, dp_lo, dp_hi, dp_dis = regime.combine(cd, w_depth, bands_d)
        rows.append({
            "symbol": r["symbol"], "spread_now": r["spread_now"], "spread_half": r["spread_now"] * np.exp(sp),
            "spread_lo": r["spread_now"] * np.exp(sp_lo), "spread_hi": r["spread_now"] * np.exp(sp_hi),
            "spread_a": r["spread_now"] * np.exp(cs["a"]), "spread_b": r["b_spread_beta1.0"],
            "spread_c": r["spread_now"] * np.exp(cs["c"]), "spread_disagree": sp_dis,
            "depth_now": r["depth_now"], "depth_half": r["depth_now"] * np.exp(dp),
            "depth_lo": r["depth_now"] * np.exp(dp_lo), "depth_hi": r["depth_now"] * np.exp(dp_hi),
            "depth_disagree": dp_dis,
            "turnover_change": float(np.exp(r["a_ln_turnover_change"])),
            "fill10_front_now": c["now"]["fill10_by_queue"][0], "fill10_front_half": c["half_even"]["fill10_by_queue"][0],
            "markout1s_now_cents": c["now"]["markout_1s_cents"], "markout1s_half_cents": c["half_even"]["markout_1s_cents"],
            "naive_cps_now": c["now"]["naive_pnl_per_share_cents"], "naive_cps_half": c["half_even"]["naive_pnl_per_share_cents"],
            "latent_share": r["latent_share"], "eta": r["eta"],
        })
    out = pl.DataFrame(rows)

    bundle = pathlib.Path(a.bundle)
    bundle.mkdir(parents=True, exist_ok=True)
    out.write_csv(bundle / "forecast.csv", float_precision=6)
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    stores = sorted(pathlib.Path(os.environ.get("HFT_DATA", pathlib.Path(os.environ.get("HFT_DATA", pathlib.Path.home() / "data")))).glob("store-S12*-v50"))
    manifest = {
        "code_commit": commit,
        "forecast_sha256": sha256(bundle / "forecast.csv"),
        "inputs": {s.name: (s / "source.sha256").read_text().strip() for s in stores if (s / "source.sha256").exists()},
        "scripts": {p: sha256(ROOT / p) for p in ("research/regime.py", "research/method_c.py", "research/qr.py",
                                                  "research/regime_report.py", "src/sources/queue_reactive.hpp",
                                                  "src/strategy/qr_events.hpp", "apps/day_stats.cpp")},
        "weights": {"spread": w_spread, "depth": w_depth}, "heldout_errors": errors,
    }
    (bundle / "manifest.json").write_text(json.dumps(manifest, indent=1))
    (bundle / "scoring.md").write_text(SCORING)

    p = rep["past_tick_changes"]
    pil = past["tick_size_pilot_tg1"]
    tk1, tk2 = past["tokyo_2014_phase1"], past["tokyo_2014_phase2"]
    lines = [
        "# MS7: regime forecast (half-penny tick, 10 mil cap)",
        "",
        f"Inputs: per-stock day statistics of the train days (fit) and the validation day (scoring at the "
        f"current tick); {rep['stocks_fit']:,} Nasdaq common stocks, {rep['stocks_treated']} treated by the "
        f"Nasdaq-only proxy (mid at least $1, time-weighted spread at most $0.015). Forecasts for the "
        f"{len(out)} treated universe stocks; the universe's {fc.filter(~pl.col('treated')).height} untreated "
        f"stocks keep the current tick. Bundle: `docs/prereg/` (forecast, manifest with hashes, scoring rules).",
        "",
        "## Method A fit",
        "",
        "| Target | Leave-stocks-out R2 | MAE (log) | Validation-day R2 |",
        "|---|---|---|---|",
        *[f"| {regime.TARGETS[k]} | {v['lso_r2']:.3f} | {v['lso_mae']:.3f} | {v['val_r2']:.3f} |" for k, v in rep["targets"].items()],
        "",
        "## Against past tick changes",
        "",
        "The causal step cannot be tested on one tick regime; the methods are checked against the direction and "
        "size of published effects. Inputs are our stocks, not the original populations.",
        "",
        "| Change | Measure | Published | Method A | Method B |",
        "|---|---|---|---|---|",
        f"| Tick Size Pilot, x5 (TG1 vs controls) | quoted spread | {pil['quoted_spread_change']:+.0%} | "
        f"{p['pilot_ln_spread_median_change']:+.0%} | {p['pilot_b_spread_median_change']:+.0%} |",
        f"| Tick Size Pilot, x5 | depth | {pil['depth_change']:+.0%} | {p['pilot_ln_depth_median_change']:+.0%} | - |",
        f"| Tokyo 2014 phase I (TOPIX 100, smaller tick) | effective spread | "
        f"{tk1['effective_spread_bps_after'] / tk1['effective_spread_bps_before'] - 1:+.0%} | "
        f"{p['halving_ln_spread_median_change_treated']:+.0%} (halving, treated) | "
        f"{p['halving_b_spread_median_change_treated']:+.0%} |",
        f"| Tokyo 2014 phase II | effective spread | "
        f"{tk2['effective_spread_bps_after'] / tk2['effective_spread_bps_before'] - 1:+.0%} | | |",
        f"| Tokyo 2014 | depth | {tk1['depth']} | {p['halving_ln_depth_median_change_treated']:+.0%} | - |",
        "",
        f"Pilot-like stocks: {p['pilot_like_stocks']} ($5 to $50, notional below the median). The Tokyo tick "
        "reductions differed by price band and were larger than a halving, so only the direction is comparable.",
        "",
        "## Combination",
        "",
        "Weights are inverse squared median absolute log errors predicting the validation day at the current "
        "tick, on the same stocks. At the current tick these stocks quote about one tick, so B and C predict "
        "the spread level almost trivially; the weights measure level accuracy, not the accuracy of the "
        "tick response, which only the past tick changes speak to.",
        "",
        "| Target | Method | Held-out error | Weight |",
        "|---|---|---|---|",
        *[f"| spread | {m} | {fmt(errors['spread'][m])} | {w_spread[m]:.2f} |" for m in "abc"],
        *[f"| depth | {m} | {fmt(errors['depth'][m])} | {w_depth[m]:.2f} |" for m in "ac"],
        "",
        "## Forecast per treated universe stock",
        "",
        "Spread in cents, depth in shares at the best (mean of the two sides). Bands span every method's "
        "estimate and its own band (A: 90% stock bootstrap; B: beta 1/2 to 1; C: even or inner-weighted "
        "redistribution). D marks methods that disagree (opposite signs or more than a factor 1.5 apart).",
        "",
        "| Stock | Spread now | Half: combined [band] | A | B | C | Depth now | Half: combined [band] | Turnover x (A) | Latent share |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in out.iter_rows(named=True):
        lines.append(
            f"| {r['symbol']} | {r['spread_now'] * 100:.3f} | {r['spread_half'] * 100:.3f} [{r['spread_lo'] * 100:.3f}, "
            f"{r['spread_hi'] * 100:.3f}]{' D' if r['spread_disagree'] else ''} | {r['spread_a'] * 100:.3f} | "
            f"{r['spread_b'] * 100:.3f} | {r['spread_c'] * 100:.3f} | {r['depth_now']:,.0f} | {r['depth_half']:,.0f} "
            f"[{r['depth_lo']:,.0f}, {r['depth_hi']:,.0f}]{' D' if r['depth_disagree'] else ''} | "
            f"{r['turnover_change']:.2f} | {r['latent_share']:.1%} |")
    lines += [
        "",
        "## Method C: fills, markouts and market-maker PnL (simulated)",
        "",
        "Probability that an order joining the best with less than one AES ahead fills within 10 s, mean 1 s "
        "markout of fills (cents, positive is good for the resting order), and the naive strategy's PnL per share "
        "in the backtest (30 mil fees now, 10 mil cap at the half tick).",
        "",
        "| Stock | Fill 10 s now | half | Markout now | half | Naive c/share now | half |",
        "|---|---|---|---|---|---|---|",
        *[f"| {r['symbol']} | {fmt(r['fill10_front_now'])} | {fmt(r['fill10_front_half'])} | "
          f"{fmt(r['markout1s_now_cents'])} | {fmt(r['markout1s_half_cents'])} | {fmt(r['naive_cps_now'])} | "
          f"{fmt(r['naive_cps_half'])} |" for r in out.iter_rows(named=True)],
        "",
        "## Limitations",
        "",
        f"- Method C fails validation at the current tick: median absolute log error of depth "
        f"{errors['depth']['c']:.2f} (a factor {np.exp(errors['depth']['c']):.1f}); simulated reference-price moves "
        f"per session range {min(mc[s]['now']['moves'] for s in mc):,} to {max(mc[s]['now']['moves'] for s in mc):,} "
        "(INTC on the validation day: 7,241 real moves). Calibrated level-1 flux is net positive at every queue size "
        "(INTC: about 9.8 AES/s in against 7.3 out), yet real level-1 queues empty about 12,300 times a day; model I "
        "has no dependence on the opposite queue (HLR models II and III), which is the next step. Its fill, "
        "markout and PnL columns are therefore not forecasts; they are reported only as the simulator's output.",
        "- Not built: the power-law feedback kernel of Noble et al., the DP policy re-solved at the new tick, and "
        "the queue value term for lower rebates. Fill probability by queue position, markouts and market-maker "
        "PnL per share have no validated forecast.",
        "- Method B covers only the spread; method A's bands are a stock bootstrap within one regime.",
        "- The Tokyo comparison uses aggregate effective spreads (CMCRC) whose tick factors differ by price band.",
        "",
        "## Latent demand inside the tick",
        "",
        f"Median share of executed shares that are hidden fills at sub-penny prices: treated "
        f"{rep['latent_share_median']['treated']:.1%}, untreated {rep['latent_share_median']['untreated']:.1%}.",
        "",
    ]
    pathlib.Path(a.out_md).write_text("\n".join(lines))
    print(f"wrote {a.out_md} and {bundle}")


SCORING = """# Scoring rules (fixed before any post-change data)

- Targets per treated stock: time-weighted quoted spread on Nasdaq and depth at the best,
  from apps/day_stats on the first post-compliance Nasdaq ITCH sample days.
- Conditioning: realized 1-minute volatility, traded notional and price on the scoring days are
  plugged into method A; changes are also scored as the difference between treated and
  untreated universe stocks.
- Errors: log error of the predicted change against the realized change per stock; coverage of
  the bands; CRPS of the combined forecast taken as uniform over its band.
- Delay: Nasdaq posts sample days irregularly; the forecast stays open until the first
  post-compliance sample day is available.
"""


if __name__ == "__main__":
    main()
