"""Statistics checks (DESIGN.md section 12): bootstrap against SciPy, CRPS against its closed
form for a normal forecast, and the deflated Sharpe ratio calibrated under the null."""

import pathlib
import sys

import numpy as np
from scipy import stats as st

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import stats  # noqa: E402


def test_cluster_bootstrap_matches_scipy_with_singleton_clusters():
    rng = np.random.default_rng(0)
    x = rng.normal(1.0, 2.0, size=400)
    m, lo, hi = stats.cluster_bootstrap(x, np.arange(len(x)), n=8000, seed=1)
    ref = st.bootstrap((x,), np.mean, n_resamples=8000, method="percentile", random_state=2).confidence_interval
    assert abs(lo - ref.low) < 0.03 and abs(hi - ref.high) < 0.03, (lo, hi, ref)


def test_cluster_bootstrap_widens_with_correlated_clusters():
    rng = np.random.default_rng(3)
    day = np.repeat(np.arange(8), 50)
    x = rng.normal(size=8)[day] + rng.normal(size=len(day)) * 0.2  # day shocks dominate
    _, lo_c, hi_c = stats.cluster_bootstrap(x, day, n=4000)
    _, lo_i, hi_i = stats.cluster_bootstrap(x, np.arange(len(x)), n=4000)
    assert hi_c - lo_c > 3 * (hi_i - lo_i)


def test_crps_matches_normal_closed_form():
    rng = np.random.default_rng(5)
    mu, sigma, y = 0.3, 1.7, 1.1
    sample = rng.normal(mu, sigma, size=200_000)
    z = (y - mu) / sigma
    closed = sigma * (z * (2 * st.norm.cdf(z) - 1) + 2 * st.norm.pdf(z) - 1 / np.sqrt(np.pi))
    assert abs(stats.crps_sample(sample, y) - closed) < 0.01


def test_deflated_sharpe_is_calibrated_under_the_null():
    """Pick the best of N unskilled strategies: DSR above 0.95 should happen about 5% of the
    time, while the undeflated PSR of the winner exceeds 0.95 far more often."""
    rng = np.random.default_rng(7)
    n_trials, t, reps = 20, 250, 400
    dsr_hits = psr_hits = 0
    for _ in range(reps):
        r = rng.normal(0, 1, size=(n_trials, t))
        srs = r.mean(axis=1) / r.std(axis=1, ddof=1)
        best = r[np.argmax(srs)]
        var_sr = np.var(srs, ddof=1)
        dsr_hits += stats.deflated_sharpe(best, n_trials, var_sr) > 0.95
        psr_hits += stats.probabilistic_sharpe(best) > 0.95
    assert dsr_hits / reps < 0.10, dsr_hits / reps
    assert psr_hits / reps > 0.4, psr_hits / reps


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
