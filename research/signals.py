"""Signal evaluation of DESIGN.md section 1 on the validation day, models fitted on train
days only.

- Spearman IC of each feature against each target, per symbol, then averaged; 95% interval
  across symbols (the stock is the unit; one validation day). Split large/small tick.
- IC decay against the horizon.
- Stoikov (2018) microprice: Markov chain on (imbalance bucket, spread) fitted on train days.
- Ridge (closed form, day-wise cross-validated penalty on train days), LightGBM (early
  stopping on the last train day) and online RLS with forgetting (started from the ridge
  weights, updated through the validation day) combinations: out-of-sample R2 and IC.
- Tradable IC: IC on rows where the predicted move exceeds half the spread plus the taker fee.

Usage: python research/signals.py <train.parquet>... --val <validation.parquet> --out <md>
"""

import argparse
import json
import pathlib
import sqlite3
import subprocess
import sys

import lightgbm as lgb
import numpy as np
import polars as pl
from scipy.stats import spearmanr

META = {"symbol", "group", "day", "seq", "ts", "mid"}
TAKER_FEE_TICKS = 0.30  # $0.0030 per share against a $0.01 tick
IMB_BUCKETS = 10


def feature_cols(df):
    return [c for c in df.columns if c not in META and not c.startswith("y_")]


def target_cols(df):
    return [c for c in df.columns if c.startswith("y_")]


def ic_by_symbol(df, x, y):
    out = []
    for _, g in df.group_by("symbol"):
        a, b = g[x].to_numpy(), g[y].to_numpy()
        ok = np.isfinite(a) & np.isfinite(b)
        if ok.sum() > 100 and np.std(a[ok]) > 0 and np.std(b[ok]) > 0:
            out.append(spearmanr(a[ok], b[ok]).statistic)
    return np.array(out)


def mean_ci(v):
    if len(v) < 2:
        return float("nan"), float("nan")
    return float(np.mean(v)), float(1.96 * np.std(v, ddof=1) / np.sqrt(len(v)))


# Stoikov microprice -------------------------------------------------------------------
def micro_states(df):
    imb = df["imbalance_l1"].to_numpy()
    spread = df["spread_ticks"].to_numpy()
    b = np.clip(((imb + 1) / 2 * IMB_BUCKETS).astype(int), 0, IMB_BUCKETS - 1)
    s = np.clip(np.nan_to_num(spread, nan=1).round().astype(int), 1, 3) - 1
    return b + IMB_BUCKETS * s, np.isfinite(imb) & np.isfinite(spread)


def fit_microprice(train):
    n = IMB_BUCKETS * 3
    Q = np.zeros((n, n))  # transitions without a mid change
    G = np.zeros(n)
    cnt = np.zeros(n)
    for _, g in train.sort("day", "symbol", "seq").group_by("day", "symbol", maintain_order=True):
        st, ok = micro_states(g)
        mid = g["mid"].to_numpy()
        a, b = st[:-1], st[1:]
        valid = ok[:-1] & ok[1:] & np.isfinite(mid[:-1]) & np.isfinite(mid[1:])
        dm = mid[1:] - mid[:-1]
        np.add.at(cnt, a[valid], 1)
        moved = valid & (dm != 0)
        np.add.at(G, a[moved], dm[moved])
        still = valid & (dm == 0)
        np.add.at(Q, (a[still], b[still]), 1)
    with np.errstate(invalid="ignore", divide="ignore"):
        Q = np.nan_to_num(Q / cnt[:, None])
        G = np.nan_to_num(G / cnt)
    # g = sum_k Q^k G: expected eventual mid change from each state.
    return np.linalg.solve(np.eye(n) - Q, G)


def add_microprice(df, g):
    st, ok = micro_states(df)
    return df.with_columns(pl.Series("microprice", np.where(ok, g[st], np.nan)))


# Models --------------------------------------------------------------------------------
def standardize(train, other, cols):
    mu = train.select([pl.col(c).mean() for c in cols]).row(0)
    sd = train.select([pl.col(c).std() for c in cols]).row(0)
    def z(d):
        # Winsorized at 5 sd: a few heavy-tailed flow features otherwise dominate linear fits.
        return np.column_stack([np.clip(np.nan_to_num((d[c].to_numpy() - m) / (s or 1)), -5, 5)
                                for c, m, s in zip(cols, mu, sd)])
    return z(train), z(other)


def ridge(X, y, lam):
    A = X.T @ X + lam * len(y) * np.eye(X.shape[1])
    return np.linalg.solve(A, X.T @ y)


def rls_predict(X, y, symbols, ts, horizon_ns, w0, lam=0.9999, delta=1.0):
    """Online recursive least squares per symbol with forgetting factor lam, started from the
    ridge weights. A row's target is used for an update only once it has resolved, at
    ts + horizon, so every prediction uses targets known at its own time."""
    p = np.empty(len(y))
    for s in np.unique(symbols):
        idx = np.flatnonzero(symbols == s)
        w = w0.copy()
        P = np.eye(len(w0)) * delta
        nxt = 0  # first row not yet used for an update
        for i in idx:
            while nxt < len(idx) and ts[idx[nxt]] + horizon_ns <= ts[i]:
                x, t = X[idx[nxt]], y[idx[nxt]]
                Px = P @ x
                g = Px / (lam + x @ Px)
                w = w + g * (t - x @ w)
                P = (P - np.outer(g, Px)) / lam
                nxt += 1
            p[i] = X[i] @ w
    return p


def r2(y, p):
    return 1 - np.sum((y - p) ** 2) / np.sum((y - np.mean(y)) ** 2)


def evaluate(train, val, cols, y, trials):
    tr = train.filter(pl.col(y).is_finite())
    va = val.filter(pl.col(y).is_finite())
    Xtr, Xva = standardize(tr, va, cols)
    ytr, yva = tr[y].to_numpy(), va[y].to_numpy()
    # Penalty chosen by leave-one-train-day-out, so validation never informs it.
    days = tr["day"].to_numpy()
    best = None
    for lam in (1e-4, 1e-3, 1e-2, 1e-1):
        trials["ridge"] += 1
        score = np.mean([r2(ytr[days == d], Xtr[days == d] @ ridge(Xtr[days != d], ytr[days != d], lam))
                         for d in np.unique(days)])
        if best is None or score > best[0]:
            best = (score, lam)
    w = ridge(Xtr, ytr, best[1])
    p_ridge = Xva @ w

    last = np.sort(np.unique(days))[-1]
    trials["lightgbm"] += 1
    model = lgb.train({"objective": "regression", "learning_rate": 0.05, "num_leaves": 31,
                       "min_data_in_leaf": 500, "feature_fraction": 0.8, "verbose": -1,
                       "seed": 1, "deterministic": True, "num_threads": 8},
                      lgb.Dataset(Xtr[days != last], ytr[days != last]), num_boost_round=500,
                      valid_sets=[lgb.Dataset(Xtr[days == last], ytr[days == last])],
                      callbacks=[lgb.early_stopping(30, verbose=False)])
    p_gbm = model.predict(Xva, num_iteration=model.best_iteration)

    preds = [("ridge", p_ridge), ("lightgbm", p_gbm)]
    if y.endswith("ms"):  # clock horizon: resolution time known; event horizons skip RLS
        trials["rls"] += 1
        h_ns = int(y[2:-2]) * 1_000_000
        preds.append(("rls", rls_predict(Xva, yva, va["symbol"].to_numpy(), va["ts"].to_numpy(), h_ns, w)))

    res = {}
    for name, p in preds:
        d = va.with_columns(pl.Series("pred", p))
        ics = ic_by_symbol(d, "pred", y)
        half = d["spread_ticks"].to_numpy() / 2 + TAKER_FEE_TICKS
        tradable = np.abs(p) > half
        tics = ic_by_symbol(d.filter(pl.Series(tradable)), "pred", y) if tradable.sum() > 0 else np.array([])
        res[name] = {"r2": float(r2(yva, p)), "ic": mean_ci(ics), "tradable_ic": mean_ci(tics),
                     "tradable_share": float(tradable.mean())}
    return res


def git_commit():
    return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                          cwd=pathlib.Path(__file__).parent).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("train", nargs="+")
    ap.add_argument("--val", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--train-every", type=int, default=6)
    ap.add_argument("--registry", default=str(pathlib.Path.home() / "data/registry.sqlite"))
    a = ap.parse_args()

    # Every k-th train row keeps the job under the 14 GB limit of N4.
    train = pl.concat([pl.read_parquet(p).gather_every(a.train_every) for p in a.train])
    val = pl.read_parquet(a.val)
    g = fit_microprice(train)
    train, val = add_microprice(train, g), add_microprice(val, g)
    cols, ys = feature_cols(val), target_cols(val)
    lines = [f"Train: {sorted(set(train['day']))}, {train.height} rows. "
             f"Validation: {sorted(set(val['day']))}, {val.height} rows, "
             f"{val['symbol'].n_unique()} symbols.", ""]

    for group in ("large_tick", "small_tick"):
        v = val.filter(pl.col("group") == group)
        lines += [f"### Feature IC, {group.replace('_', '-')} ({v['symbol'].n_unique()} symbols)", "",
                  "Mean over symbols of the per-symbol Spearman IC, with 95% interval across symbols.", "",
                  "| Feature | " + " | ".join(ys) + " |", "|---|" + "---|" * len(ys)]
        for c in cols:
            cells = []
            for y in ys:
                m, h = mean_ci(ic_by_symbol(v, c, y))
                cells.append(f"{m:+.3f} ± {h:.3f}")
            lines.append(f"| {c} | " + " | ".join(cells) + " |")
        lines.append("")

    trials = {"ridge": 0, "lightgbm": 0, "rls": 0}
    lines += ["### Combined signals (fitted on train days, scored on validation)", "",
              "| Group | Target | Model | OOS R2 | IC | Tradable IC | Tradable share |",
              "|---|---|---|---|---|---|---|"]
    summary = {}
    for group in ("large_tick", "small_tick"):
        tr, va = train.filter(pl.col("group") == group), val.filter(pl.col("group") == group)
        for y in ys:
            res = evaluate(tr, va, cols, y, trials)
            summary[f"{group}/{y}"] = res
            for name, r in res.items():
                lines.append(f"| {group} | {y} | {name} | {r['r2']:+.4f} | {r['ic'][0]:+.3f} ± {r['ic'][1]:.3f} "
                             f"| {r['tradable_ic'][0]:+.3f} ± {r['tradable_ic'][1]:.3f} | {r['tradable_share']:.3f} |")
    lines += ["", f"Model fits tried: {trials['ridge']} ridge penalties, {trials['lightgbm']} LightGBM "
              f"fits, {trials['rls']} RLS runs."]
    pathlib.Path(a.out).write_text("\n".join(lines) + "\n")

    with sqlite3.connect(a.registry) as db:
        db.execute("""CREATE TABLE IF NOT EXISTS research_runs (run_id INTEGER PRIMARY KEY,
                      started TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), question TEXT,
                      script TEXT, commit_hash TEXT, inputs TEXT, trials INTEGER, metrics TEXT)""")
        db.execute("INSERT INTO research_runs (question, script, commit_hash, inputs, trials, metrics) "
                   "VALUES (?, ?, ?, ?, ?, ?)",
                   ("Q1-signals", "research/signals.py", git_commit(), json.dumps(a.train + [a.val]),
                    sum(trials.values()), json.dumps(summary)))
    print("\n".join(lines[-12:]))


if __name__ == "__main__":
    main()
