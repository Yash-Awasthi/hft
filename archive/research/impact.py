"""Impact from public order-level data (DESIGN.md section 6).

- Trade-sign autocorrelation C(l) and response R(l) = E[eps_t (m_{t+l} - m_t)] in trade time.
- Propagator (Bouchaud, Gefen, Potters, Wyart 2004): with m_t = sum_{s<t} G(t - s) eps_s + noise,
  R(l) = sum_{n>=1} G(n) (C(l - n) - C(n)), C symmetric; solved for G(1..L) by least squares.
- Hurst exponent of the mid in trade time from the variance of increments over lags.
- Square-root law: I / sigma_D = Y (Q / V_D)^psi fitted by log-log regression over bins of
  Q / V_D, with a bootstrap over metaorders.
- Transient impact with kernel G: the round-trip cost x' Gamma x / 2 with Gamma_ij = G(|i - j|)
  must be non-negative (Gatheral 2010), checked by the smallest eigenvalue; the cost-minimizing
  schedule is x = Gamma^{-1} 1 X / (1' Gamma^{-1} 1) (Gatheral, Schied and Slynko 2012).

Usage: python research/impact.py <py-build> --stores <store>... --symbols ... --out <dir>
"""

import argparse
import json
import pathlib
import sys
import tomllib

import numpy as np
from scipy.linalg import toeplitz

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sign_acf(eps, lmax):
    """Autocovariance of the signs at lags 0..lmax (biased, demeaned), by FFT."""
    x = np.asarray(eps, float) - np.mean(eps)
    n = len(x)
    f = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[: lmax + 1] / (n - np.arange(lmax + 1))
    return ac


def response(eps, m, lmax):
    """R(l) for l = 1..lmax: E[eps_t (m_{t+l} - m_t)]."""
    eps, m = np.asarray(eps, float), np.asarray(m, float)
    return np.array([np.mean(eps[:-l] * (m[l:] - m[:-l])) for l in range(1, lmax + 1)])


def design_matrix(C, L, K=None):
    """A with R(1..L) = A G(1..K) (K = L by default), from C(0..) normalized so C(0) = 1."""
    C = np.asarray(C, float) / C[0]
    Cfull = lambda k: C[np.minimum(np.abs(k), len(C) - 1)] * (np.abs(k) < len(C))
    l = np.arange(1, L + 1)[:, None]
    n = np.arange(1, (K or L) + 1)[None, :]
    return Cfull(l - n) - Cfull(n)


def propagator(R, C, L):
    """G(1..L) from R(1..L) and C(0..L) by unconstrained least squares."""
    return np.linalg.lstsq(design_matrix(C, L), np.asarray(R[:L], float), rcond=None)[0]


def power_law_kernel(p, L):
    """G(0..L) = G0 (1 + l / l0)^-beta."""
    return p["G0"] * (1 + np.arange(L + 1) / p["l0"]) ** -p["beta"]


def propagator_power_law(R, C, L):
    """G0, l0, beta of G(l) = G0 (1 + l / l0)^-beta fitted to R(1..L) by bounded least squares,
    the kernel summed over every lag of C (a kernel cut at L biases the fit). G0 >= 0 and beta >= 0 make G positive, decreasing and convex, so every Toeplitz matrix of it
    is positive semi-definite (Polya) and the no-arbitrage check holds by construction."""
    from scipy.optimize import least_squares

    K = len(C) - 1
    A, R = design_matrix(C, L, K), np.asarray(R[:L], float)
    scale = max(float(np.abs(R).max()), 1e-300)
    lag = np.arange(1, K + 1)
    resid = lambda x: (A @ (x[0] * (1 + lag / np.exp(x[1])) ** -x[2]) - R) / scale
    best = None
    for l0 in (1.0, 10.0, 100.0):
        for beta in (0.1, 0.5):
            x0 = [max(R[0], 0.0) + 1e-12, np.log(l0), beta]
            r = least_squares(resid, x0, bounds=([0, np.log(0.1), 0], [np.inf, np.log(1e4), 3]))
            if best is None or r.cost < best.cost:
                best = r
    G0, ll0, beta = best.x
    return {"G0": float(G0), "l0": float(np.exp(ll0)), "beta": float(beta),
            "rmse": float(np.sqrt(2 * best.cost / L) * scale)}


def hurst(m, lags=(1, 2, 4, 8, 16, 32, 64, 128, 256)):
    v = [np.var(m[l:] - m[:-l]) for l in lags]
    return float(np.polyfit(np.log(lags), np.log(v), 1)[0] / 2)


def fit_power(q, imp, bins=12, n_boot=500, seed=0):
    """psi and Y of mean(imp | q) = Y q^psi over quantile bins of q; 95% bootstrap band of psi.
    Logs of noisy bin means bias psi upwards when the smallest bins are noisy."""
    q, imp = np.asarray(q, float), np.asarray(imp, float)

    def one(qq, ii):
        edges = np.quantile(qq, np.linspace(0, 1, bins + 1))
        idx = np.clip(np.searchsorted(edges, qq, side="right") - 1, 0, bins - 1)
        xs, ys = [], []
        for b in range(bins):
            sel = idx == b
            mi = ii[sel].mean() if sel.sum() > 5 else -1
            if mi > 0:
                xs.append(np.log(qq[sel]).mean())
                ys.append(np.log(mi))
        if len(xs) < 3:
            return np.nan, np.nan
        psi, c = np.polyfit(xs, ys, 1)
        return psi, np.exp(c)

    psi, Y = one(q, imp)
    rng = np.random.default_rng(seed)
    draws = [one(q[k], imp[k])[0] for k in (rng.integers(0, len(q), len(q)) for _ in range(n_boot))]
    lo, hi = np.nanquantile(draws, [0.025, 0.975])
    return float(psi), float(lo), float(hi), float(Y)


def sign_runs(sign, min_len=2):
    """Start and end indices (inclusive) of runs of equal signs at least min_len long."""
    sign = np.asarray(sign)
    cut = np.flatnonzero(np.diff(sign) != 0) + 1
    starts, ends = np.r_[0, cut], np.r_[cut - 1, len(sign) - 1]
    keep = ends - starts + 1 >= min_len
    return starts[keep], ends[keep]


def min_eigenvalue(G):
    return float(np.linalg.eigvalsh(toeplitz(np.asarray(G, float))).min())


def optimal_schedule(G, X):
    """Minimum-cost split of X over len(G) equal slots under kernel G; (x, cost, twap cost)."""
    Gam = toeplitz(np.asarray(G, float))
    w = np.linalg.solve(Gam, np.ones(len(G)))
    x = w * X / w.sum()
    twap = np.full(len(G), X / len(G))
    return x, float(x @ Gam @ x / 2), float(twap @ Gam @ twap / 2)


def attributed_metaorders(ex, gap_ns=60_000_000_000, min_execs=3):
    """Runs of same-sign executions against one attributed participant's resting orders, a
    new run starting after a gap: (first index, last index, shares) into the executions."""
    out = []
    ids = ex["mpid"]
    for pid in np.unique(ids[ids > 0]):
        idx = np.flatnonzero(ids == pid)
        start = idx[0]
        for a, b in zip(idx[:-1], idx[1:]):
            if ex["sign"][b] != ex["sign"][a] or ex["ts"][b] - ex["ts"][a] > gap_ns:
                out.append((start, a))
                start = b
        out.append((start, idx[-1]))
    return [(s, e, int(ex["shares"][[i for i in range(s, e + 1) if ids[i] == ids[s]]].sum()))
            for s, e in out if (ids[s:e + 1] == ids[s]).sum() >= min_execs]


def mid_at(ts_query, ts, mid_before, mid_after):
    """Mid just after the last trade at or before each query time."""
    k = np.searchsorted(ts, ts_query, side="right") - 1
    return np.where(k >= 0, mid_after[np.maximum(k, 0)], mid_before[0])


def analyze_symbol(hftpy, stores, sym, start, end, lmax=500):
    days = []
    for store in stores:
        t = hftpy.trades(store, hftpy.symbols(store)[sym], start, end)
        ok = np.isfinite(t["mid_before"]) & np.isfinite(t["mid_after"])
        days.append({k: (t[k][ok] if k != "executions" else t[k]) for k in t})
    eps = np.concatenate([d["sign"].astype(float) for d in days])
    C = np.mean([sign_acf(d["sign"].astype(float), lmax) for d in days], axis=0)
    R = np.mean([response(d["sign"].astype(float), d["mid_before"], lmax) for d in days], axis=0)
    G = propagator(R, C, lmax)
    H = float(np.mean([hurst(d["mid_before"]) for d in days]))
    # Metaorders: sign runs of trades and attributed passive execution runs.
    runs, attr = [], []
    for d in days:
        vol = float(d["shares"].sum())
        r = np.diff(d["mid_before"])
        sig = float(np.sqrt(np.sum(r ** 2))) / float(np.mean(d["mid_before"]))  # daily, relative
        s, e = sign_runs(d["sign"], 2)
        q = np.add.reduceat(d["shares"], s)[: len(s)] if len(s) else np.array([])
        q = np.array([d["shares"][a:b + 1].sum() for a, b in zip(s, e)])
        imp = d["sign"][s] * (d["mid_after"][e] - d["mid_before"][s]) / d["mid_before"][s]
        runs.append((q / vol, imp / sig))
        ex = d["executions"]
        for a, b, sh in attributed_metaorders(ex):
            m0 = mid_at(ex["ts"][a] - 1, d["ts"], d["mid_before"], d["mid_after"])
            m1 = mid_at(ex["ts"][b], d["ts"], d["mid_before"], d["mid_after"])
            attr.append((sh / vol, ex["sign"][a] * (m1 - m0) / m0 / sig))
    q_r = np.concatenate([x[0] for x in runs])
    i_r = np.concatenate([x[1] for x in runs])
    out = {"symbol": sym, "trades": int(len(eps)), "mean_sign": float(eps.mean()),
           "C": (C / C[0])[:lmax + 1].tolist(), "R": R.tolist(), "G": G.tolist(), "hurst": H,
           "kernel_min_eig": min_eigenvalue(np.r_[G[0], G]), "sign_runs": int(len(q_r)),
           "attributed_runs": len(attr), "G_power": propagator_power_law(R, C, lmax // 2)}
    lags = np.arange(1, lmax + 1)
    good = (C[1:] / C[0]) > 0
    out["kappa"] = float(-np.polyfit(np.log(lags[good][9:200]), np.log((C[1:] / C[0])[good][9:200]), 1)[0])
    out["psi_runs"] = fit_power(q_r, i_r, n_boot=200)
    if len(attr) > 50:
        qa, ia = np.array(attr).T
        out["psi_attributed"] = fit_power(qa, ia, n_boot=200)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_build")
    ap.add_argument("--stores", nargs="+", required=True)
    ap.add_argument("--symbols", nargs="*")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sys.path.insert(0, a.py_build)
    import hftpy

    uni = tomllib.loads((ROOT / "configs/universe.toml").read_text())
    groups = {s: g for g in ("large_tick", "small_tick") for s in uni[g]["symbols"]}
    syms = a.symbols or list(groups)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    res = []
    for s in syms:
        r = analyze_symbol(hftpy, a.stores, s, 34_500_000_000_000, 57_300_000_000_000)
        r["group"] = groups.get(s)
        res.append(r)
        print(s, r["trades"], f"H={r['hurst']:.3f} kappa={r['kappa']:.2f} psi_runs={r['psi_runs'][0]:.2f}",
              flush=True)
    (out / "impact.json").write_text(json.dumps(res))


if __name__ == "__main__":
    main()
