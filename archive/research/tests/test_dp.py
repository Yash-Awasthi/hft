"""DP solver cross-checks (DESIGN.md section 12): value iteration against brute force over
every deterministic stationary policy of a tiny MDP, and queue outcomes on hand cases."""

import itertools
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import dp  # noqa: E402


def tiny_mdp(rng, n_s=2):
    P, R = [], []
    for s in range(n_s):
        row_p, row_r = [], []
        for a in range(dp.N_ACTIONS):
            if a not in (0, 4, dp.TAKE_BUY):  # three actions available
                row_p.append(None)
                row_r.append(0.0)
                continue
            k = 3
            prob = rng.dirichlet(np.ones(k))
            s2 = rng.integers(0, n_s, size=k)
            fb = rng.integers(0, 2, size=k) if a == 4 else np.zeros(k, dtype=int)
            fa = np.zeros(k, dtype=int)
            dm = rng.normal(size=k) * 0.5
            row_p.append((prob, s2, fb, fa, dm))
            row_r.append(rng.normal())
        P.append(row_p)
        R.append(row_r)
    return P, R


def policy_value(P, R, pi, Q, phi, discount, take_cost):
    """Exact value of a deterministic policy by solving the linear system."""
    qs = list(range(-Q, Q + 1))
    n = len(qs) * len(P)
    idx = lambda q, s: (q + Q) * len(P) + s
    A = np.eye(n)
    b = np.zeros(n)
    for q in qs:
        for s in range(len(P)):
            a = pi[q + Q, s]
            prob, s2, fb, fa, dm = P[s][a]
            q2 = np.clip(q + fb - fa + (1 if a == dp.TAKE_BUY else 0), -Q, Q)
            out = (q + fb - fa + (1 if a == dp.TAKE_BUY else 0) > Q)
            b[idx(q, s)] = R[s][a] + np.sum(prob * (q2 * dm - phi * q2 ** 2)) - 1e6 * np.sum(prob[out])
            if a == dp.TAKE_BUY:
                b[idx(q, s)] -= take_cost
            for p_, s2_, q2_ in zip(prob, s2, q2):
                A[idx(q, s), idx(q2_, s2_)] -= discount * p_
    return np.linalg.solve(A, b).reshape(len(qs), len(P))


def test_value_iteration_matches_brute_force():
    rng = np.random.default_rng(4)
    P, R = tiny_mdp(rng)
    Q, phi, disc, cost = 1, 0.05, 0.9, 0.3
    V, pi, _ = dp.value_iteration(P, R, Q, phi, disc, cost, tol=1e-12)
    actions = (0, 4, dp.TAKE_BUY)
    best = None
    cells = (2 * Q + 1) * len(P)
    for choice in itertools.product(actions, repeat=cells):
        pol = np.array(choice).reshape(2 * Q + 1, len(P))
        v = policy_value(P, R, pol, Q, phi, disc, cost)
        best = v if best is None else np.maximum(best, v)
    assert np.allclose(V, best, atol=1e-6), (V, best)
    assert np.allclose(policy_value(P, R, pi, Q, phi, disc, cost), best, atol=1e-6)


def test_side_outcomes_on_hand_cases():
    L = np.array([100.0, 100.0, 100.0, 100.0])
    exe = np.array([95.0, 10.0, 0.0, 0.0])
    can = np.array([0.0, 0.0, 50.0, 0.0])
    moved = np.array([0, 0, 0, 1])
    nxt, fill = dp.side_outcome(dp.NONE, dp.BEST, L, exe, can, moved, np.ones(4), np.array([5.0, 90.0, 50.0, 100.0]))
    assert list(fill) == [False, False, False, False]  # joining behind 100 shares
    # 5 and 90 shares left ahead: one lot or less, so the front; 50 ahead after pro-rata cancels.
    assert nxt[0] == dp.FRONT and nxt[1] == dp.FRONT and nxt[2] == dp.FRONT and nxt[3] == dp.NONE
    L2 = np.array([1000.0])
    nxt, fill = dp.side_outcome(dp.NONE, dp.BEST, L2, np.array([100.0]), np.array([0.0]), np.array([0]),
                                np.ones(1), np.array([1500.0]))
    assert nxt[0] == dp.MIDDLE and not fill[0]  # 900 ahead of 1500
    nxt, fill = dp.side_outcome(dp.FRONT, dp.BEST, L, exe, can, moved, np.ones(4), L)
    assert list(fill) == [True, False, False, False]  # 15 ahead: only the 95-share sweep reaches us
    nxt, fill = dp.side_outcome(dp.NONE, dp.INSIDE, L, np.array([1.0, 0, 0, 0]), can, moved, np.array([2, 2, 1, 2]), L)
    assert list(fill) == [True, False, False, False]
    assert nxt[1] == dp.FRONT and nxt[3] == dp.FRONT


def test_policy_iteration_matches_value_iteration():
    rng = np.random.default_rng(11)
    P, R = tiny_mdp(rng, n_s=4)
    Q, phi, disc, cost = 2, 0.03, 0.95, 0.2
    V1, pi1, _ = dp.value_iteration(P, R, Q, phi, disc, cost, tol=1e-12)
    V2, pi2, _ = dp.policy_iteration(dp.from_lists(P, R), Q, phi, disc, cost)
    assert np.allclose(V1, V2, atol=1e-6)
    assert np.array_equal(pi1, pi2)


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
