"""Queue-reactive model calibration (Huang, Lehalle and Rosenbaum 2015, model I; DESIGN.md
section 5, method C) from the event record of hftpy.qr_events.

Queue sizes are in units of the average event size (AES) of the stock, n = ceil(shares / AES)
capped at N. Rates per level i and size n, pooled over the two sides:
  L_i(n) = insertions / time with n at level i,
  C_i(n) = cancellations / time with n at level i,
  M_i(n) = executions / time with n at level i while every level before it on its side is empty.
Insertions at the instant of a reference move (the new levels forming) belong to the move and
are not counted. theta = moved / (moved + refilled) over depletion episodes of level 1. The
redraw distribution is the time-weighted distribution of n per level.
"""

import numpy as np


def calibrate(ev, K, N, tick=100):
    kind, side, level = ev["kind"], ev["side"].astype(int), ev["level"].astype(int)
    ts = ev["ts"].astype(np.int64)
    counted = (ev["after_move"] == 0) & (kind < 3)
    aes = int(round(float(np.mean(ev["shares"][counted]))))
    q = np.minimum(np.ceil(ev["q"] / aes), N).astype(int)  # (events, 2K): bid 1..K, ask 1..K
    dt = np.diff(ts, prepend=ts[0]) * 1e-9  # state before record j holds since record j-1
    dt[0] = 0.0

    time = np.zeros((K, N + 1))
    time_m = np.zeros((K, N + 1))
    for s in range(2):
        qs = q[:, s * K:(s + 1) * K]
        empty_before = np.ones(len(q), bool)
        for i in range(K):
            np.add.at(time[i], qs[:, i], dt)
            np.add.at(time_m[i], qs[:, i][empty_before], dt[empty_before])
            empty_before &= qs[:, i] == 0

    counts = {k: np.zeros((K, N + 1)) for k in "LCM"}
    for code, name in ((0, "L"), (1, "C"), (2, "M")):
        sel = counted & (kind == code)
        n_own = q[sel, side[sel] * K + level[sel] - 1]
        np.add.at(counts[name], (level[sel] - 1, n_own), 1)

    out = {"aes": aes, "K": K, "N": N}
    for name, t in (("L", time), ("C", time), ("M", time_m)):
        with np.errstate(divide="ignore", invalid="ignore"):
            rate = np.where(t > 0, counts[name] / t, 0.0)
            se = np.where(t > 0, np.sqrt(np.maximum(counts[name], 1)) / t, 0.0)
        out[name], out[name + "_se"], out["time_" + name], out["n_" + name] = rate, se, t, counts[name]
    out["init"] = time / np.maximum(time.sum(axis=1, keepdims=True), 1e-300)
    moved, refilled = int(ev["episodes_moved"]), int(ev["episodes_refilled"])
    tot = max(moved + refilled, 1)
    out["theta"] = moved / tot
    out["theta_se"] = float(np.sqrt(out["theta"] * (1 - out["theta"]) / tot))
    out["seconds"] = float(dt.sum())
    return out


def simulate_args(fit, p_ref, start_ns, end_ns, seed, path, fill_unvisited=True):
    """Arguments of hftpy.qr_simulate from a calibration. States never visited get the rate of
    the nearest visited size of the same level, so the simulator never stalls there."""
    K, N = fit["K"], fit["N"]
    tabs = {}
    for name in "LCM":
        r = fit[name].copy()
        t = fit["time_" + name]
        if fill_unvisited:
            for i in range(K):
                seen = np.flatnonzero(t[i] > 0)
                if len(seen):
                    r[i] = r[i][seen[np.abs(np.arange(N + 1)[:, None] - seen[None, :]).argmin(axis=1)]]
        tabs[name] = r.ravel().tolist()
    init = fit["init"].ravel().tolist()
    return dict(K=K, N=N, aes=fit["aes"], p_ref=p_ref, theta=fit["theta"], L=tabs["L"], C=tabs["C"],
                M=tabs["M"], init=init, start_ns=start_ns, end_ns=end_ns, seed=seed, path=str(path))
