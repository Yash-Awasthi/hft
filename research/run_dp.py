"""Builds and solves the quoting MDP per tick group from train-day grids, then exports the
policy tables served by the C++ DpPolicy strategy.

For each group: 100 ms grids of every universe symbol on the train days (hftpy.grid); a ridge
signal on the grid features predicting the mid change over the next second; market states
from imbalance, spread and signal buckets; empirical transitions (dp.estimate); exact policy
iteration. Two tables per group: without the signal (strategy 3) and with it (strategy 4).

Usage: python research/run_dp.py <py-build> <out-dir> <train-store>...
"""

import json
import pathlib
import sys
import time
import tomllib

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research"))
import dp  # noqa: E402

STEP_NS = 100_000_000
START, END = 34_200_000_000_000 + 300_000_000_000, 57_600_000_000_000 - 300_000_000_000
HORIZON_STEPS = 10
REBATE_TICKS, FEE_TICKS = 0.20, 0.30
Q, PHI, DISCOUNT = 5, 0.01, 0.999


def grids(hftpy, stores, symbols):
    out = []
    for store in stores:
        sym = hftpy.symbols(store)
        for s in symbols:
            if s in sym:
                g = hftpy.grid(store, sym[s], [sym["SPY"], sym["QQQ"]], STEP_NS, START, END)
                out.append(g)
    return out


def pairs(gs, signal, edges):
    """Consecutive steps (t, t+1) of each grid joined into one sample set."""
    cols = {k: [] for k in ("bid", "ask", "qty_b", "qty_a", "exe_b", "exe_a", "can_b", "can_a",
                            "mov_b", "mov_a", "qty_b2", "qty_a2", "x", "x2", "dm")}
    for g, sig in zip(gs, signal):
        bid, ask = g["bid_px"] / 100.0, g["ask_px"] / 100.0
        imb, spread = g["X"][:, 1], ask - bid
        x = dp.market_state(imb, spread, sig, edges)
        mid = (bid + ask) / 2
        ok = np.isfinite(g["mid_end"][:-1]) & (np.diff(g["ts"]) == STEP_NS)
        sl = lambda a: a[:-1][ok]
        cols["bid"].append(sl(bid)); cols["ask"].append(sl(ask))
        cols["qty_b"].append(sl(g["qty"][:, 0])); cols["qty_a"].append(sl(g["qty"][:, 1]))
        cols["exe_b"].append(sl(g["exec"][:, 0])); cols["exe_a"].append(sl(g["exec"][:, 1]))
        cols["can_b"].append(sl(g["cancel"][:, 0])); cols["can_a"].append(sl(g["cancel"][:, 1]))
        cols["mov_b"].append(sl(g["moved"][:, 0])); cols["mov_a"].append(sl(g["moved"][:, 1]))
        cols["qty_b2"].append(g["qty"][1:, 0][ok]); cols["qty_a2"].append(g["qty"][1:, 1][ok])
        cols["x"].append(sl(x)); cols["x2"].append(x[1:][ok])
        cols["dm"].append((mid[1:] - mid[:-1])[ok])
    return {k: np.concatenate(v) for k, v in cols.items()}


def fit_signal(gs):
    """Ridge on standardized grid features predicting the mid change HORIZON_STEPS ahead."""
    X, y = [], []
    for g in gs:
        mid = (g["bid_px"] + g["ask_px"]) / 200.0
        fut = np.full(len(mid), np.nan)
        fut[:-HORIZON_STEPS] = mid[HORIZON_STEPS:] - mid[:-HORIZON_STEPS]
        X.append(g["X"]); y.append(fut)
    X, y = np.concatenate(X), np.concatenate(y)
    ok = np.isfinite(y) & np.isfinite(X).all(axis=1)
    X, y = X[ok], y[ok]
    mu, sd = X.mean(axis=0), X.std(axis=0) + 1e-12
    Z = np.clip((X - mu) / sd, -5, 5)
    w = np.linalg.solve(Z.T @ Z + 1e-2 * len(y) * np.eye(Z.shape[1]), Z.T @ y)
    pred = Z @ w
    edges = np.quantile(pred, [1 / 3, 2 / 3])
    r2 = 1 - np.sum((y - pred) ** 2) / np.sum((y - y.mean()) ** 2)
    return {"weights": w, "means": mu, "stds": sd, "edges": edges}, float(r2)


def fit_intensity(gs):
    """Fill intensity Lambda(d) = A exp(-k d) for a one-lot quote d ticks from the mid, from
    how often any execution happens at the touch in a step, by half-spread: lambda =
    -ln(1 - P(execution)) / step. Log-linear least squares over half-spreads with data."""
    d_all, hit_all = [], []
    for g in gs:
        half = (g["ask_px"] - g["bid_px"]) / 200.0
        for k in range(2):
            d_all.append(half)
            hit_all.append(g["exec"][:, k] > 0)
    d, hit = np.concatenate(d_all), np.concatenate(hit_all)
    xs, ys = [], []
    for v in np.unique(np.round(d * 2) / 2):
        sel = np.round(d * 2) / 2 == v
        if sel.sum() < 1000:
            continue
        p = min(hit[sel].mean(), 0.999)
        xs.append(v)
        ys.append(np.log(-np.log1p(-p) / (STEP_NS * 1e-9)))
    if len(xs) < 2:
        return float(np.exp(ys[0])) if ys else 1.0, 1.5
    k, logA = np.polyfit(xs, ys, 1)
    return float(np.exp(logA)), float(-k)


def signal_of(gs, sig):
    return [np.clip((np.nan_to_num(g["X"]) - sig["means"]) / sig["stds"], -5, 5) @ sig["weights"] for g in gs]


def main():
    py_build, out = sys.argv[1], pathlib.Path(sys.argv[2])
    stores = sys.argv[3:]
    sys.path.insert(0, py_build)
    import hftpy

    out.mkdir(parents=True, exist_ok=True)
    universe = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    for group in ("large_tick", "small_tick"):
        t0 = time.time()
        gs = grids(hftpy, stores, universe[group]["symbols"])
        sig, r2 = fit_signal(gs)
        A, k = fit_intensity(gs)
        sigs = signal_of(gs, sig)
        for name, use_signal in (("nosignal", False), ("signal", True)):
            # Without the signal every state falls in the middle bucket, here and in C++.
            edges = sig["edges"] if use_signal else np.array([-np.inf, np.inf])
            st = pairs(gs, sigs, edges)
            model, tot = dp.estimate(st, REBATE_TICKS, FEE_TICKS)
            V, pi, iters = dp.policy_iteration(model, Q, PHI, DISCOUNT, 0.0)
            meta = {"group": group, "variant": name, "train": stores, "samples": int(len(st["dm"])),
                    "signal_r2_train": r2, "policy_iterations": int(iters), "Q": Q, "phi": PHI,
                    "discount": DISCOUNT, "rebate_ticks": REBATE_TICKS, "fee_ticks": FEE_TICKS,
                    "step_ns": STEP_NS, "action_share": np.bincount(pi.ravel(), minlength=dp.N_ACTIONS).tolist(),
                    "value_flat_mean": float(V[Q].mean()), "as_A": A, "as_k": k}
            used = {**sig, "edges": edges}
            dp.export(out / f"dp_{group}_{name}.bin", pi, Q, used, meta)
            print(json.dumps(meta), f"{time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
