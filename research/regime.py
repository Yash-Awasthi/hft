"""Regime forecast (DESIGN.md section 5): tick $0.01 -> $0.005 and the 10 mil fee cap.

Inputs: apps/day_stats output per day. Per stock, over the days given: mean mid, spread,
depth at the best, queue turnover (displayed shares executed per second over depth at the
best), eta, 1-minute volatility of the mid, traded notional, and the share of executions
against hidden orders at sub-penny prices (latent demand inside the tick).

Method A, cross-sectional tick elasticity: with x = ln(tick / sigma_1min) the tick in units of
one-minute volatility, a ridge regression of each log target on a cubic B-spline of x plus
ln notional and ln mid, on dimensionless targets (spread in ticks, depth relative to volume,
executions relative to depth). A change of tick by `factor` moves each stock along the curve:
predicted log change = f(x + ln factor) - f(x) (+ ln factor for the spread in dollars), added
to the stock's own observed value.

Method B, implicit spread (Dayri and Rosenbaum 2015): eta = eta0 (alpha0 / alpha)^(1 - beta/2)
with beta in {1/2, 1}; for tick-constrained stocks (eta0 < 1/2) the new spread is the larger of
the new tick and the implicit spread 2 eta alpha; otherwise only the new tick can bind.

Usage: python research/regime.py --fit <train.tsv>... --check <val.tsv> --out <dir>
"""

import argparse
import json
import pathlib

import numpy as np
import polars as pl
from scipy.interpolate import BSpline

TICK = 0.01
SECONDS = 23_400.0
# Dimensionless targets, so that the cross-section identifies a tick effect rather than a
# volatility or volume effect: spread in ticks, depth over shares traded per minute, and
# displayed shares executed per day over depth. Only the spread target carries the tick unit.
TARGETS = {"ln_spread": "spread in ticks", "ln_depth": "depth at best / shares per minute",
           "ln_turnover": "displayed executions per day / depth"}
TICK_UNIT = {"ln_spread": 1.0, "ln_depth": 0.0, "ln_turnover": 0.0}
KNOTS = np.linspace(-6, 4, 9)


def load(paths):
    days = []
    for p in paths:
        d = pl.read_csv(p, separator="\t", null_values=["nan", "-nan"], infer_schema_length=20000)
        days.append(d)
    df = pl.concat(days, how="vertical_relaxed")
    df = df.filter((pl.col("etp") == "N") & (pl.col("two_sided_share") >= 0.95) & (pl.col("mid") >= 1)
                   & (pl.col("executions") >= 200) & (pl.col("depth") > 0) & (pl.col("rv_1min") > 0))
    agg = df.group_by("symbol").agg(
        pl.len().alias("days"), pl.col("mid").mean(), pl.col("spread_ticks").mean(),
        pl.col("depth").mean(), pl.col("notional").mean(), pl.col("rv_1min").mean(),
        pl.col("displayed").mean(), pl.col("hidden").mean(), pl.col("subpenny_hidden").mean(),
        pl.col("eta").mean(), pl.col("one_tick_share").mean(), pl.col("shares").mean())
    return agg.filter(pl.col("days") == len(paths)).with_columns(
        spread=pl.col("spread_ticks") * TICK,
        sigma=(pl.col("rv_1min") / 389).sqrt(),
        turnover=pl.col("displayed") / SECONDS / pl.col("depth"),
        latent_share=pl.col("subpenny_hidden") / (pl.col("displayed") + pl.col("hidden")),
    ).with_columns(
        x=(TICK / pl.col("sigma")).log(), ln_notional=pl.col("notional").log(), ln_mid=pl.col("mid").log(),
        ln_spread=pl.col("spread_ticks").log(), ln_depth=(pl.col("depth") * 390 / pl.col("shares")).log(),
        ln_turnover=(pl.col("displayed") / pl.col("depth")).log(),
    ).filter(pl.col("x").is_finite() & pl.col("ln_turnover").is_finite()).sort("symbol")


def _basis(x):
    k = 3
    t = np.r_[[KNOTS[0]] * k, KNOTS, [KNOTS[-1]] * k]
    xc = np.clip(x, KNOTS[0], KNOTS[-1])
    n = len(t) - k - 1
    return np.column_stack([BSpline(t, np.eye(n)[i], k)(xc) for i in range(n)])


def _design(df, shift=0.0):
    return np.column_stack([_basis(df["x"].to_numpy() + shift), df["ln_notional"].to_numpy(),
                            df["ln_mid"].to_numpy()])


def fit_a(df, target, lam=1e-3):
    X, y = _design(df), df[target].to_numpy()
    mu, sd = X.mean(axis=0), X.std(axis=0) + 1e-12
    Z = (X - mu) / sd
    w = np.linalg.solve(Z.T @ Z + lam * len(y) * np.eye(Z.shape[1]), Z.T @ (y - y.mean()))
    return {"w": w, "mu": mu, "sd": sd, "b": float(y.mean())}


def predict_a(m, df, shift=0.0):
    return ((_design(df, shift) - m["mu"]) / m["sd"]) @ m["w"] + m["b"]


def change_a(m, df, log_factor, unit=0.0):
    """Predicted log change of the target when the tick is multiplied by exp(log_factor); with
    unit = 1 the change of a quantity measured in ticks, converted to dollars."""
    return predict_a(m, df, log_factor) - predict_a(m, df) + unit * log_factor


def leave_stocks_out(df, target, folds=5, seed=0):
    rng = np.random.default_rng(seed)
    fold = rng.integers(0, folds, len(df))
    pred = np.empty(len(df))
    for f in range(folds):
        m = fit_a(df.filter(pl.Series(fold != f)), target)
        pred[fold == f] = predict_a(m, df.filter(pl.Series(fold == f)))
    y = df[target].to_numpy()
    return float(1 - np.sum((y - pred) ** 2) / np.sum((y - y.mean()) ** 2)), float(np.mean(np.abs(y - pred)))


def eta_after(eta0, factor, beta):
    return eta0 * factor ** (-(1 - beta / 2))


def spread_b(spread0, eta0, tick0, factor, beta):
    tick = tick0 * factor
    if eta0 < 0.5:
        return max(tick, 2 * eta_after(eta0, factor, beta) * tick)
    return max(tick, spread0)


def weights(errors):
    """Combination weights from each method's held-out error at the current tick (inverse
    squared); a method without a held-out score gets no weight."""
    inv = {k: (1 / e ** 2 if e else 0.0) for k, e in errors.items()}
    tot = sum(inv.values())
    return {k: v / tot for k, v in inv.items()}


def combine(changes, w, bands):
    """Weighted log change; the band spans every method's estimate and band, and the methods
    disagree when their estimates differ in sign or by more than a factor of 1.5."""
    vals = [v for v in changes.values() if v is not None and np.isfinite(v)]
    comb = sum(w.get(k, 0) * v for k, v in changes.items() if v is not None and np.isfinite(v))
    comb /= max(sum(w.get(k, 0) for k, v in changes.items() if v is not None and np.isfinite(v)), 1e-12)
    pts = vals + [x for b in bands.values() for x in b]
    disagree = (min(vals) < 0 < max(vals)) or (max(vals) - min(vals) > np.log(1.5))
    return float(comb), float(min(pts)), float(max(pts)), bool(disagree)


def bootstrap_change(df, target, log_factor, rows, n=200, seed=1):
    """Percentile band of the predicted change for `rows`, refitting on resampled stocks."""
    rng = np.random.default_rng(seed)
    out = np.empty((n, len(rows)))
    for i in range(n):
        m = fit_a(df[rng.integers(0, len(df), len(df))], target)
        out[i] = change_a(m, rows, log_factor, TICK_UNIT[target])
    return np.quantile(out, [0.05, 0.95], axis=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", nargs="+", required=True)
    ap.add_argument("--check", required=True)
    ap.add_argument("--universe", default=str(pathlib.Path(__file__).resolve().parents[1] / "configs/universe.toml"))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    import tomllib

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    fit, check = load(a.fit), load([a.check])
    uni = tomllib.loads(pathlib.Path(a.universe).read_text())
    members = {s: g for g in ("large_tick", "small_tick") for s in uni[g]["symbols"]}
    treated = (pl.col("mid") >= 1) & (pl.col("spread") <= 0.015)

    report = {"stocks_fit": len(fit), "stocks_treated": int(fit.filter(treated).height), "targets": {}}
    models = {}
    for t in TARGETS:
        models[t] = fit_a(fit, t)
        r2, mae = leave_stocks_out(fit, t)
        # Out of sample in time: fitted on the train days, scored on the validation day.
        common = check.join(fit.select("symbol"), on="symbol")
        pred = predict_a(models[t], common)
        y = common[t].to_numpy()
        r2_val = float(1 - np.sum((y - pred) ** 2) / np.sum((y - y.mean()) ** 2))
        report["targets"][t] = {"lso_r2": r2, "lso_mae": mae, "val_r2": r2_val}

    # Forecast per universe stock under the half-penny tick.
    rows = fit.filter(pl.col("symbol").is_in(list(members)))
    fc = {"symbol": rows["symbol"].to_list(), "group": [members[s] for s in rows["symbol"]],
          "treated": rows.select(treated).to_series().to_list(), "mid": rows["mid"].to_list(), "spread_now": rows["spread"].to_list(),
          "depth_now": rows["depth"].to_list(), "turnover_now": rows["turnover"].to_list(),
          "eta": rows["eta"].to_list(), "latent_share": rows["latent_share"].to_list()}
    for t in TARGETS:
        d = change_a(models[t], rows, np.log(0.5), TICK_UNIT[t])
        lo, hi = bootstrap_change(fit, t, np.log(0.5), rows)
        fc[f"a_{t}_change"], fc[f"a_{t}_lo"], fc[f"a_{t}_hi"] = d.tolist(), lo.tolist(), hi.tolist()
    for beta in (0.5, 1.0):
        fc[f"b_spread_beta{beta}"] = [spread_b(s, e, TICK, 0.5, beta) if np.isfinite(e) else None
                                      for s, e in zip(rows["spread"], rows["eta"])]
    fc = pl.DataFrame(fc)
    fc.write_parquet(out / "forecast_half_tick.parquet")

    # Past tick changes: the Pilot quintupled the tick for small caps; apply the same factor to
    # stocks like its test group (price $5-$50, notional below the median).
    pilot_like = fit.filter((pl.col("mid") >= 5) & (pl.col("mid") <= 50)
                            & (pl.col("notional") < fit["notional"].median()))
    past = {"pilot_like_stocks": pilot_like.height}
    for t in ("ln_spread", "ln_depth"):
        past[f"pilot_{t}_median_change"] = float(np.median(np.expm1(change_a(models[t], pilot_like, np.log(5), TICK_UNIT[t]))))
    past["pilot_b_spread_median_change"] = float(np.median(
        [spread_b(s, e, TICK, 5.0, 1.0) / s - 1 for s, e in zip(pilot_like["spread"], pilot_like["eta"])
         if np.isfinite(e)]))
    tokyo_like = fit.filter(treated)
    for t in ("ln_spread", "ln_depth"):
        past[f"halving_{t}_median_change_treated"] = float(np.median(np.expm1(change_a(models[t], tokyo_like, np.log(0.5), TICK_UNIT[t]))))
    past["halving_b_spread_median_change_treated"] = float(np.median(
        [spread_b(s, e, TICK, 0.5, 1.0) / s - 1 for s, e in zip(tokyo_like["spread"], tokyo_like["eta"])
         if np.isfinite(e)]))
    report["past_tick_changes"] = past
    report["latent_share_median"] = {
        "treated": float(fit.filter(treated)["latent_share"].median()),
        "untreated": float(fit.filter(~treated)["latent_share"].median())}
    (out / "regime.json").write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
