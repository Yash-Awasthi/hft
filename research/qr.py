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


def _accumulate(ev, K, N, aes):
    kind, side, level = ev["kind"], ev["side"].astype(int), ev["level"].astype(int)
    ts = ev["ts"].astype(np.int64)
    counted = (ev["after_move"] == 0) & (kind < 3)
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
    return time, time_m, counts, int(ev["episodes_moved"]), int(ev["episodes_refilled"]), float(dt.sum())


def calibrate(evs, K, N, tick=100, aes=None):
    """Pooled over the event records `evs` (one per day, or a single record)."""
    evs = [evs] if isinstance(evs, dict) else list(evs)
    if aes is None:
        sizes = np.concatenate([e["shares"][(e["after_move"] == 0) & (e["kind"] < 3)] for e in evs])
        aes = int(round(float(np.mean(sizes))))
    time, time_m = np.zeros((K, N + 1)), np.zeros((K, N + 1))
    counts = {k: np.zeros((K, N + 1)) for k in "LCM"}
    moved = refilled = 0
    seconds = 0.0
    for e in evs:
        t, tm, c, mv, rf, sec = _accumulate(e, K, N, aes)
        time += t
        time_m += tm
        for k in counts:
            counts[k] += c[k]
        moved, refilled, seconds = moved + mv, refilled + rf, seconds + sec
    out = {"aes": aes, "K": K, "N": N, "tick": tick}
    for name, t in (("L", time), ("C", time), ("M", time_m)):
        with np.errstate(divide="ignore", invalid="ignore"):
            rate = np.where(t > 0, counts[name] / t, 0.0)
            se = np.where(t > 0, np.sqrt(np.maximum(counts[name], 1)) / t, 0.0)
        out[name], out[name + "_se"], out["time_" + name], out["n_" + name] = rate, se, t, counts[name]
    out["init"] = time / np.maximum(time.sum(axis=1, keepdims=True), 1e-300)
    tot = max(moved + refilled, 1)
    out["theta"] = moved / tot
    out["theta_se"] = float(np.sqrt(out["theta"] * (1 - out["theta"]) / tot))
    out["seconds"] = seconds
    return out


def half_tick(fit, inner_share=0.5):
    """The calibrated model at half the tick (method C). Each old level i becomes new levels
    2i-1 and 2i; insertions split inner_share / 1 - inner_share between them, the cancellation
    and market-order rates keep their dependence on the own queue size, theta is kept for a
    half-tick move, and redraws use the old level's distribution scaled by the share."""
    K, N = fit["K"], fit["N"]
    out = {k: fit[k] for k in ("aes", "N", "theta")}
    out["K"], out["tick"] = 2 * K, fit["tick"] // 2
    for name in "LCM":
        out[name] = np.repeat(fit[name], 2, axis=0)
        out["time_" + name] = np.repeat(fit["time_" + name], 2, axis=0)
    share = np.tile([inner_share, 1 - inner_share], K)
    out["L"] = out["L"] * share[:, None]
    init = np.zeros((2 * K, N + 1))
    n = np.arange(N + 1)
    for j in range(2 * K):
        moved = np.minimum(np.round(n * share[j]).astype(int), N)
        np.add.at(init[j], moved, fit["init"][j // 2])
    out["init"] = init
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
                M=tabs["M"], init=init, start_ns=start_ns, end_ns=end_ns, seed=seed, path=str(path),
                tick=fit["tick"])
