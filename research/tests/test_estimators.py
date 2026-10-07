"""Parameter recovery for research estimators (DESIGN.md section 12): each fitted on data
simulated with known parameters must recover them within tolerance."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import hawkes  # noqa: E402


def test_hawkes_recovers_parameters():
    rng = np.random.default_rng(7)
    truth = (2.0, 0.6, 25.0)
    t = hawkes.simulate(*truth, horizon=4000.0, rng=rng)
    mu, alpha, beta = hawkes.fit(t, 4000.0)
    assert abs(mu - truth[0]) / truth[0] < 0.1, mu
    assert abs(alpha - truth[1]) < 0.05, alpha
    assert abs(beta - truth[2]) / truth[2] < 0.15, beta


def test_hawkes_loglik_matches_reference_recursion():
    rng = np.random.default_rng(3)
    t = hawkes.simulate(1.0, 0.4, 5.0, horizon=200.0, rng=rng)
    p = (1.0, 0.4, 5.0)
    assert abs(hawkes.loglik(p, t, 200.0) - hawkes._loglik_fast(p, t, 200.0)) < 1e-6


def test_rls_predictions_ignore_unresolved_targets():
    import signals  # noqa: E402
    rng = np.random.default_rng(1)
    n, k = 400, 4
    X = rng.normal(size=(n, k))
    y = X @ np.array([1.0, -0.5, 0.2, 0.0]) + rng.normal(size=n)
    ts = np.cumsum(rng.integers(1, 1_000_000, size=n))
    sym = np.array(["A"] * n)
    h = 5_000_000
    p = signals.rls_predict(X, y, sym, ts, h, np.zeros(k))
    cut = 250
    y2 = y.copy()
    # Targets not yet resolved at row `cut` may change without moving predictions up to it.
    unresolved = ts + h > ts[cut]
    y2[unresolved] = rng.normal(size=unresolved.sum()) * 100
    p2 = signals.rls_predict(X, y2, sym, ts, h, np.zeros(k))
    assert np.array_equal(p[: cut + 1], p2[: cut + 1])
    assert not np.array_equal(p, p2)


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
