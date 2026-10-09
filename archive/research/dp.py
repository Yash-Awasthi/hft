"""Data-driven optimal quoting (DESIGN.md section 3).

A discrete MDP in the spirit of Guilbaud and Pham (2013) and Cartea, Jaimungal and Ricci
(2014), estimated from a 100 ms grid of real data (hftpy.grid) and solved exactly by value
iteration.

State: inventory q in lots (-Q..Q), queue bucket of our bid and of our ask
(none / front / middle / back by the share of the level ahead of us), and the market state x
(imbalance bucket, spread of one or more ticks, signal bucket).
Actions: per side out / at the best / one tick inside (when the spread allows); or take one
lot with a market order. Transitions and rewards are counted from the grid, replaying a
hypothetical one-lot order at each queue position against the real executions and cancels
of each step. Reward per step in ticks: spread capture of fills, rebates, fees, inventory
times the mid change, minus phi q^2.
"""

import numpy as np

# Queue buckets by the share of the level ahead of us, except that one lot or less ahead
# always counts as the front: a 5-share level ahead fills us on almost any trade.
NONE, FRONT, MIDDLE, BACK = 0, 1, 2, 3
REP = {FRONT: 0.15, MIDDLE: 0.5, BACK: 0.9}  # representative share ahead
LOT = 100
OUT, BEST, INSIDE = 0, 1, 2
SIDE_ACTIONS = 3
TAKE_BUY, TAKE_SELL = 9, 10
N_ACTIONS = 11


def queue_bucket(rho, ahead):
    return np.where((rho <= 1 / 3) | (ahead <= LOT), FRONT, np.where(rho <= 2 / 3, MIDDLE, BACK))


def side_outcome(pos, act, L, exe, can, moved, spread_ticks, L_next):
    """Vectorized over samples: next queue bucket and fill flag of a one-lot order on one side.

    pos: current bucket (scalar), act: OUT/BEST/INSIDE (scalar); arrays per sample:
    L level size at the start, exe/can shares executed/cancelled at that price in the step,
    moved -1 taken out / +1 a better price appeared / 0 held, L_next level size after.
    """
    n = len(L)
    nxt = np.full(n, NONE)
    fill = np.zeros(n, dtype=bool)
    if act == OUT:
        return nxt, fill
    inside = act == INSIDE
    if inside:
        # Alone at a new better price: any aggressor on our side reaches us first.
        ok = spread_ticks >= 2
        fill = ok & ((exe > 0) | (moved == -1))
        nxt = np.where(ok & ~fill, FRONT, NONE)
        # Without room inside, INSIDE acts as BEST.
        if ok.all():
            return nxt, fill
        n_nxt, n_fill = side_outcome(pos, BEST, L, exe, can, moved, spread_ticks, L_next)
        return np.where(ok, nxt, n_nxt), np.where(ok, fill, n_fill)
    rho = 1.0 if pos == NONE else REP[pos]
    ahead = rho * L if pos != FRONT else np.minimum(rho * L, LOT)
    fill = exe >= ahead + 1e-9
    fill |= (moved == -1) & (exe >= ahead)
    left = np.maximum(ahead - exe - can * np.divide(ahead, L, out=np.zeros(n), where=L > 0), 0)
    rho_next = np.divide(left, np.maximum(L_next, 1), out=np.zeros(n), where=True)
    nxt = np.where(fill, NONE, queue_bucket(np.clip(rho_next, 0, 1), left))
    # A better price on our side leaves us behind the touch: we drop out and may rejoin.
    nxt = np.where(~fill & (moved == 1), NONE, nxt)
    # Our level emptied without reaching us: we are now first in it.
    nxt = np.where(~fill & (moved == -1), FRONT, nxt)
    return nxt, fill


def value_iteration(P, R, Q, phi, discount, take_cost, tol=1e-9, max_iter=100_000):
    """Exact value iteration.

    P[s, a]: list of (prob, s2, fb, fa, dm) arrays (dense over transitions); R[s, a]: expected
    spread-and-fee reward in ticks excluding inventory terms. Inventory q in -Q..Q; fills
    move q by +fb - fa; reward adds q' dm - phi q'^2. Take actions move q by one lot and pay
    take_cost. Returns V[q, s] and the greedy policy pi[q, s].
    """
    n_s = len(P)
    qs = np.arange(-Q, Q + 1)
    V = np.zeros((len(qs), n_s))
    for it in range(max_iter):
        Qv = np.full((len(qs), n_s, N_ACTIONS), -np.inf)
        for s in range(n_s):
            for a in range(N_ACTIONS):
                tr = P[s][a]
                if tr is None:
                    continue
                prob, s2, fb, fa, dm = tr
                for iq, q in enumerate(qs):
                    q2 = q + fb - fa + (1 if a == TAKE_BUY else -1 if a == TAKE_SELL else 0)
                    ok = (q2 >= -Q) & (q2 <= Q)
                    if not ok.any():
                        continue
                    q2c = np.clip(q2, -Q, Q)
                    val = R[s][a] + np.sum(prob * (q2c * dm - phi * q2c ** 2 + discount * V[q2c + Q, s2]))
                    if not ok.all():
                        val -= 1e6 * np.sum(prob[~ok])
                    if a in (TAKE_BUY, TAKE_SELL):
                        val -= take_cost
                    Qv[iq, s, a] = val
        V2 = Qv.max(axis=2)
        if np.max(np.abs(V2 - V)) < tol:
            V = V2
            break
        V = V2
    return V, Qv.argmax(axis=2), it


# Vectorized model -------------------------------------------------------------------------
class Model:
    """Transitions as flat arrays: for row r, from state s[r] under action a[r], probability
    p[r] to state s2[r] with inventory change dq[r] (lots) and mean mid change dm[r] (ticks).
    R[s, a] is the expected spread, rebate and fee reward of the step; valid[s, a] marks
    actions with data."""

    def __init__(self, n_s, s, a, p, s2, dq, dm, R, valid):
        self.n_s, self.s, self.a, self.p, self.s2, self.dq, self.dm = n_s, s, a, p, s2, dq, dm
        self.R, self.valid = R, valid


def from_lists(P, R):
    rows = {k: [] for k in ("s", "a", "p", "s2", "dq", "dm")}
    Rm = np.zeros((len(P), N_ACTIONS))
    valid = np.zeros((len(P), N_ACTIONS), dtype=bool)
    for s, row in enumerate(P):
        for a, tr in enumerate(row):
            if tr is None:
                continue
            prob, s2, fb, fa, dm = tr
            take = 1 if a == TAKE_BUY else -1 if a == TAKE_SELL else 0
            rows["s"].append(np.full(len(prob), s))
            rows["a"].append(np.full(len(prob), a))
            rows["p"].append(prob)
            rows["s2"].append(s2)
            rows["dq"].append(fb - fa + take)
            rows["dm"].append(dm)
            Rm[s, a] = R[s][a]
            valid[s, a] = True
    c = {k: np.concatenate(v) for k, v in rows.items()}
    return Model(len(P), c["s"], c["a"], c["p"], c["s2"], c["dq"], c["dm"], Rm, valid)


def q_values(m, V, Q, phi, discount, take_cost):
    """Q[q, s, a] for all states at once."""
    qs = np.arange(-Q, Q + 1)
    q2 = qs[:, None] + m.dq[None, :]
    ok = (q2 >= -Q) & (q2 <= Q)
    q2c = np.clip(q2, -Q, Q)
    contrib = m.p * (q2c * m.dm - phi * q2c ** 2 + discount * V[q2c + Q, m.s2] - 1e6 * ~ok)
    out = np.zeros((len(qs), m.n_s * N_ACTIONS))
    key = m.s * N_ACTIONS + m.a
    for i in range(len(qs)):
        out[i] = np.bincount(key, weights=contrib[i], minlength=m.n_s * N_ACTIONS)
    Qv = out.reshape(len(qs), m.n_s, N_ACTIONS) + m.R[None]
    Qv[:, :, TAKE_BUY] -= take_cost
    Qv[:, :, TAKE_SELL] -= take_cost
    Qv[:, ~m.valid] = -np.inf
    return Qv


def policy_iteration(m, Q, phi, discount, take_cost, max_iter=200):
    """Exact solution: evaluate each policy with a sparse linear solve, improve greedily, stop
    when the policy is stable."""
    from scipy.sparse import coo_matrix, identity
    from scipy.sparse.linalg import spsolve

    nq = 2 * Q + 1
    n = nq * m.n_s
    V = np.zeros((nq, m.n_s))
    pi = np.argmax(np.where(m.valid, 0.0, -np.inf), axis=1)[None, :].repeat(nq, axis=0)
    qs = np.arange(-Q, Q + 1)
    for it in range(max_iter):
        # Rows of the chosen action for every (q, s).
        rows_r, cols_r, vals_r = [], [], []
        b = np.zeros(n)
        for iq, q in enumerate(qs):
            chosen = m.a == pi[iq, m.s]
            s, p, s2, dq, dm = m.s[chosen], m.p[chosen], m.s2[chosen], m.dq[chosen], m.dm[chosen]
            q2 = q + dq
            ok = (q2 >= -Q) & (q2 <= Q)
            q2c = np.clip(q2, -Q, Q)
            src = iq * m.n_s + s
            b += np.bincount(src, weights=p * (q2c * dm - phi * q2c ** 2 - 1e6 * ~ok), minlength=n)
            rows_r.append(src)
            cols_r.append((q2c + Q) * m.n_s + s2)
            vals_r.append(discount * p)
            act = pi[iq]
            b[iq * m.n_s + np.arange(m.n_s)] += m.R[np.arange(m.n_s), act] - take_cost * np.isin(act, (TAKE_BUY, TAKE_SELL))
        T = coo_matrix((np.concatenate(vals_r), (np.concatenate(rows_r), np.concatenate(cols_r))), shape=(n, n)).tocsr()
        V = spsolve((identity(n, format="csr") - T).tocsc(), b).reshape(nq, m.n_s)
        Qv = q_values(m, V, Q, phi, discount, take_cost)
        # Keep the current action on ties so the iteration stops.
        cur = np.take_along_axis(Qv, pi[:, :, None], axis=2)[:, :, 0]
        new = np.where(Qv.max(axis=2) > cur + 1e-9, Qv.argmax(axis=2), pi)
        if np.array_equal(new, pi):
            return V, pi, it
        pi = new
    raise RuntimeError("policy iteration did not converge")


# Estimation from grid data -------------------------------------------------------------------
IMB_EDGES = np.array([-0.6, -0.2, 0.2, 0.6])
SPREAD_EDGES = np.array([1.5, 3.5])  # ticks: 1, 2-3, 4+
N_IMB, N_SPREAD, N_SIG = 5, 3, 3


def market_state(imbalance, spread_ticks, signal, sig_edges):
    i = np.searchsorted(IMB_EDGES, np.nan_to_num(imbalance))
    s = np.searchsorted(SPREAD_EDGES, spread_ticks)
    g = np.searchsorted(sig_edges, np.nan_to_num(signal)) if sig_edges is not None else np.ones(len(i), int)
    return (i * N_SPREAD + s) * N_SIG + g


def n_states():
    return 16 * N_IMB * N_SPREAD * N_SIG


def estimate(steps, rebate_ticks, fee_ticks):
    """steps: dict of arrays for consecutive grid steps (t, t+1) of one or more symbol-days,
    already joined: bid, ask (ticks), qty_b/qty_a, exe_b/exe_a, can_b/can_a, mov_b/mov_a,
    qty_b2/qty_a2 (next step), x, x2 (market states), dm (mid change). Returns a Model."""
    n_x = N_IMB * N_SPREAD * N_SIG
    spread = steps["ask"] - steps["bid"]
    mid = (steps["ask"] + steps["bid"]) / 2
    rew_sum, tot = np.zeros(n_states() * N_ACTIONS), np.zeros(n_states() * N_ACTIONS)
    parts_k, parts_c, parts_d = [], [], []
    for pb in range(4):
        for pa in range(4):
            s = (pb * 4 + pa) * n_x + steps["x"]
            actions = [(ab, aa) for ab in range(SIDE_ACTIONS) for aa in range(SIDE_ACTIONS)] + ["tb", "ts"]
            for k, act in enumerate(actions):
                if isinstance(act, tuple):
                    ab, aa = act
                    nb, fb = side_outcome(pb, ab, steps["qty_b"], steps["exe_b"], steps["can_b"], steps["mov_b"], spread, steps["qty_b2"])
                    na, fa = side_outcome(pa, aa, steps["qty_a"], steps["exe_a"], steps["can_a"], steps["mov_a"], spread, steps["qty_a2"])
                    px_b = steps["bid"] + (ab == INSIDE) * (spread >= 2)
                    px_a = steps["ask"] - (aa == INSIDE) * (spread >= 2)
                    r = fb * (mid - px_b + rebate_ticks) + fa * (px_a - mid + rebate_ticks)
                    a = ab * SIDE_ACTIONS + aa
                    dq = fb.astype(int) - fa.astype(int)
                else:
                    nb = na = np.zeros(len(mid), int)
                    a = TAKE_BUY if act == "tb" else TAKE_SELL
                    r = -(spread / 2 + fee_ticks)
                    dq = np.full(len(mid), 1 if act == "tb" else -1)
                s2 = (nb * 4 + na) * n_x + steps["x2"]
                row = s * N_ACTIONS + a
                np.add.at(rew_sum, row, r)
                np.add.at(tot, row, 1)
                # Transition key: (row, s2, dq) -> count and summed mid change.
                key = (row.astype(np.int64) * n_states() + s2) * 3 + (dq + 1)
                u, inv = np.unique(key, return_inverse=True)
                parts_k.append(u)
                parts_c.append(np.bincount(inv))
                parts_d.append(np.bincount(inv, weights=steps["dm"]))
    keys, inv = np.unique(np.concatenate(parts_k), return_inverse=True)
    cnt = np.bincount(inv, weights=np.concatenate(parts_c))
    dmsum = np.bincount(inv, weights=np.concatenate(parts_d))
    dq = keys % 3 - 1
    rest = keys // 3
    s2 = rest % n_states()
    row = rest // n_states()
    s, a = row // N_ACTIONS, row % N_ACTIONS
    p = cnt / tot[row]
    dm = dmsum / cnt
    R = np.divide(rew_sum, tot, out=np.zeros_like(rew_sum), where=tot > 0).reshape(n_states(), N_ACTIONS)
    valid = (tot > 0).reshape(n_states(), N_ACTIONS)
    # A resting order cannot be "kept" where none exists: BEST from NONE means join; fine.
    return Model(n_states(), s, a, p, s2, dq, dm, R, valid), tot.reshape(n_states(), N_ACTIONS)


def export(path, pi, Q, sig, meta):
    """Binary policy table for the C++ DpPolicy: header, signal model, then pi[q][s] bytes.
    sig: dict with weights, means, stds (length k), edges (2) and feature count k."""
    import struct
    k = len(sig["weights"])
    with open(path, "wb") as f:
        f.write(b"HFTDP001")
        f.write(struct.pack("<iiiiii", Q, 16, N_IMB, N_SPREAD, N_SIG, k))
        f.write(np.asarray(IMB_EDGES, "<f8").tobytes())
        f.write(np.asarray(SPREAD_EDGES, "<f8").tobytes())
        f.write(np.asarray(sig["edges"], "<f8").tobytes())
        f.write(np.asarray(sig["means"], "<f8").tobytes())
        f.write(np.asarray(sig["stds"], "<f8").tobytes())
        f.write(np.asarray(sig["weights"], "<f8").tobytes())
        f.write(np.asarray(pi, np.uint8).tobytes())
    with open(str(path) + ".json", "w") as f:
        import json
        json.dump(meta, f, indent=1)
