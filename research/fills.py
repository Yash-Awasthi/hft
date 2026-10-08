"""Fill model, adverse selection and queue value (DESIGN.md section 2).

- Lifecycles of sampled real orders joining the best quote (C++, hftpy.lifecycles): each
  ends by its first fill, by the price moving away (a better price on its side), or is
  censored (own cancel, horizon). Cancels are censoring, an informative one for real orders.
- Competing risks: cause-specific Cox models for fill and away, fitted on train days; the
  cumulative incidence of a fill by tau is predicted for validation-day orders and compared
  with the Aalen-Johansen estimate within deciles of the prediction (calibration on held-out
  real orders).
- Markouts theta (m(t + tau) - p) of fills, by fill mechanism (sweep of the whole level or
  not, cancel within 1 ms after the fill) and by queue position at arrival.
- Queue value W(pos) = P(fill within 60 s | pos) * (E[markout at 1 s | fill, pos] + rebate).

Usage: python research/fills.py <py-build> --train <store>... --val <store> --out <md>
"""

import argparse
import pathlib
import sys
import tomllib

import numpy as np
import polars as pl
from lifelines import AalenJohansenFitter, CoxPHFitter

ROOT = pathlib.Path(__file__).resolve().parents[1]
TAUS = [0.001, 0.01, 0.1, 1.0, 10.0, 60.0]
CIF_TAUS = [0.01, 0.1, 1.0, 10.0]
HORIZON = 60.0
REBATE_TICKS = 0.20  # $0.0020 per share against a $0.01 tick
MAX_FIT_ROWS = 300_000


def collect(hftpy, store, sample):
    universe = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    groups = {s: g for g in ("large_tick", "small_tick") for s in universe[g]["symbols"]}
    sym = hftpy.symbols(store)
    names = list(hftpy.lifecycle_covariates)
    orders, fills = [], []
    for s, g in groups.items():
        if s not in sym:
            continue
        d = hftpy.lifecycles(store, sym[s], sample, HORIZON, TAUS)
        cov = d["covariates"]
        o = pl.DataFrame({"symbol": s, "group": g, "side": d["side"], "outcome": d["outcome"],
                          "duration": d["duration_s"], **{n: cov[:, i] for i, n in enumerate(names)}})
        orders.append(o)
        f = d["fills"]
        fills.append(pl.DataFrame({"symbol": s, "group": g, "sweep": f["sweep"],
                                   "cancel_after": f["cancel_after"], "shares": f["shares"],
                                   "queue_ahead": f["queue_ahead"],
                                   **{f"m{t}": f["markout"][:, i] for i, t in enumerate(TAUS)}}))
    return pl.concat(orders), pl.concat(fills)


def design(df):
    side = df["side"].to_numpy().astype(float)
    return pl.DataFrame({
        "log_ahead": np.log1p(df["queue_ahead"].to_numpy()),
        "log_opposite": np.log1p(df["opposite_qty"].to_numpy()),
        "imbalance_own": np.nan_to_num(df["imbalance"].to_numpy() * side),
        "spread": np.nan_to_num(df["spread_ticks"].to_numpy(), nan=1.0),
        "volatility": np.nan_to_num(df["volatility"].to_numpy()),
        "signal_own": np.clip(np.nan_to_num(df["ofi_signal"].to_numpy() * side / 1e3), -10, 10),
        "log_shares": np.log1p(df["shares"].to_numpy()),
        "small_tick": (df["group"] == "small_tick").cast(float).to_numpy(),
    })


def fit_cause(X, df, cause):
    data = X.with_columns(pl.Series("T", df["duration"].to_numpy()),
                          pl.Series("E", (df["outcome"] == cause).cast(int).to_numpy())).to_pandas()
    m = CoxPHFitter(penalizer=1e-3)
    m.fit(data, duration_col="T", event_col="E")
    return m


def predicted_cif(models, X, taus, chunk=20_000):
    """CIF of a fill by each tau from the two cause-specific Cox models, on a log time grid."""
    grid = np.concatenate([[0.0], np.geomspace(1e-6, max(taus), 400)])
    h0s = []
    for m in models:
        base = m.baseline_cumulative_hazard_.iloc[:, 0]
        h0s.append(np.interp(grid, base.index.values, base.values, left=0.0))
    risks = [np.exp(m.predict_log_partial_hazard(X.to_pandas()).to_numpy()) for m in models]
    cols = [np.searchsorted(grid, t, side="right") - 1 for t in taus]
    out = np.empty((len(X), len(taus)))
    for lo in range(0, len(X), chunk):
        hi = min(lo + chunk, len(X))
        H_fill = np.outer(risks[0][lo:hi], h0s[0])
        H_away = np.outer(risks[1][lo:hi], h0s[1])
        S = np.exp(-(H_fill + H_away))
        # Left-point rule on the grid: S just before each step times the fill hazard step.
        cif = np.cumsum(S[:, :-1] * np.diff(H_fill, axis=1), axis=1)
        cif = np.concatenate([np.zeros((hi - lo, 1)), cif], axis=1)
        out[lo:hi] = cif[:, cols]
    return out


def observed_cif(df, tau):
    aj = AalenJohansenFitter(calculate_variance=False)
    aj.fit(df["duration"].to_numpy(), df["outcome"].to_numpy(), event_of_interest=1)
    c = aj.cumulative_density_
    return float(np.interp(tau, c.index.values, c.iloc[:, 0].values))


def mean_ci(v):
    v = v[np.isfinite(v)]
    return np.mean(v), 1.96 * np.std(v, ddof=1) / np.sqrt(max(len(v), 2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--val", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sample", type=int, default=20)
    a = ap.parse_args()
    sys.path.insert(0, a.py_build)
    import hftpy

    tr = [collect(hftpy, s, a.sample) for s in a.train]
    train = pl.concat([o for o, _ in tr])
    train_fills = pl.concat([f for _, f in tr])
    val, val_fills = collect(hftpy, a.val, a.sample)
    rel = lambda s: f"<store-dir>/{pathlib.Path(s).name}"
    lines = [f"Train orders: {train.height} ({[rel(s) for s in a.train]}); "
             f"validation orders: {val.height} ({rel(a.val)}). "
             f"One order in {a.sample} sampled by reference; horizon {HORIZON:.0f} s.", ""]
    counts = val.group_by("outcome").len().sort("outcome")
    lines += ["Validation outcomes (0 censored, 1 fill, 2 away): " +
              ", ".join(f"{r[0]}: {r[1]}" for r in counts.iter_rows()), ""]

    fit = train.sample(min(MAX_FIT_ROWS, train.height), seed=1)
    Xfit = design(fit)
    # Winsorize at train quantiles so a few extreme covariates cannot blow up the hazards.
    lims = {c: (float(Xfit[c].quantile(0.005)), float(Xfit[c].quantile(0.995))) for c in Xfit.columns}
    clip = lambda X: X.with_columns([pl.col(c).clip(*lims[c]) for c in X.columns])
    Xfit = clip(Xfit)
    m_fill, m_away = fit_cause(Xfit, fit, 1), fit_cause(Xfit, fit, 2)
    lines += ["### Cause-specific Cox models (train days)", "",
              "| Covariate | Fill log-HR | Away log-HR |", "|---|---|---|"]
    for c in Xfit.columns:
        lines.append(f"| {c} | {m_fill.params_[c]:+.3f} ± {1.96 * m_fill.standard_errors_[c]:.3f} "
                     f"| {m_away.params_[c]:+.3f} ± {1.96 * m_away.standard_errors_[c]:.3f} |")
    lines.append("")

    cif = predicted_cif([m_fill, m_away], clip(design(val)), CIF_TAUS)
    lines += ["### Calibration on validation-day real orders", "",
              "Predicted fill probability by tau (cumulative incidence) against the Aalen-Johansen "
              "estimate, by decile of the prediction.", "",
              "| tau | Decile | Predicted | Observed | Orders |", "|---|---|---|---|---|"]
    for j, tau in enumerate(CIF_TAUS):
        p = cif[:, j]
        edges = np.quantile(p, np.linspace(0, 1, 11))
        dec = np.clip(np.searchsorted(edges, p, side="right") - 1, 0, 9)
        err = []
        for k in range(10):
            sel = val.filter(pl.Series(dec == k))
            if sel.height < 50:
                continue
            obs = observed_cif(sel, tau)
            pred = float(p[dec == k].mean())
            err.append(abs(obs - pred))
            lines.append(f"| {tau:g} s | {k + 1} | {pred:.3f} | {obs:.3f} | {sel.height} |")
        lines.append(f"| {tau:g} s | mean abs error | {np.mean(err):.3f} | | |")
    lines.append("")

    lines += ["### Markouts of fills (validation day), ticks", "",
              "theta (m(t + tau) - p); negative means adverse selection. 95% intervals.", "",
              "| Group | Mechanism | Fills | " + " | ".join(f"{t:g} s" for t in TAUS) + " |",
              "|---|---|---|" + "---|" * len(TAUS)]
    for g in ("large_tick", "small_tick"):
        for name, cond in (("all", pl.lit(True)), ("sweep", pl.col("sweep") == 1),
                           ("front, not sweep", pl.col("sweep") == 0),
                           ("cancel within 1 ms", pl.col("cancel_after") == 1)):
            f = val_fills.filter((pl.col("group") == g) & cond)
            cells = [f"{m:+.3f} ± {h:.3f}" for m, h in (mean_ci(f[f"m{t}"].to_numpy()) for t in TAUS)]
            lines.append(f"| {g} | {name} | {f.height} | " + " | ".join(cells) + " |")
    lines.append("")

    lines += ["### Queue value by queue position at arrival (validation day)", "",
              "W = P(fill within 60 s) x (markout at 1 s + rebate of 0.20 ticks).", "",
              "| Group | Shares ahead | Orders | P(fill) | Markout 1 s | W (ticks) |", "|---|---|---|---|---|---|"]
    for g in ("large_tick", "small_tick"):
        o = val.filter(pl.col("group") == g)
        f = val_fills.filter(pl.col("group") == g)
        edges = np.unique(np.quantile(o["queue_ahead"].to_numpy(), [0, 0.2, 0.4, 0.6, 0.8, 1.0]))
        for lo, hi in zip(edges[:-1], edges[1:]):
            oo = o.filter((pl.col("queue_ahead") >= lo) & (pl.col("queue_ahead") <= hi))
            ff = f.filter((pl.col("queue_ahead") >= lo) & (pl.col("queue_ahead") <= hi))
            if oo.height < 50:
                continue
            pf = observed_cif(oo, HORIZON - 1e-9)
            mk = float(np.nanmean(ff["m1.0"].to_numpy())) if ff.height else float("nan")
            lines.append(f"| {g} | {lo:.0f}-{hi:.0f} | {oo.height} | {pf:.3f} | {mk:+.3f} | "
                         f"{pf * (mk + REBATE_TICKS):+.4f} |")
    pathlib.Path(a.out).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
