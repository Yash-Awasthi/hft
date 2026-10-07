"""Impact estimators on synthetic data with known answers (DESIGN.md section 6)."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import impact  # noqa: E402


def long_memory_signs(n, d=0.3, seed=0):
    rng = np.random.default_rng(seed)
    k = np.arange(1, 4000)
    psi = np.r_[1.0, np.cumprod((k - 1 + d) / k)]  # ARFIMA(0, d, 0) weights
    x = np.convolve(rng.normal(size=n + len(psi)), psi, mode="valid")[:n]
    return np.sign(x).astype(float)


def test_sign_acf_matches_direct_sum():
    eps = long_memory_signs(20000)
    c = impact.sign_acf(eps, 10)
    direct = [np.mean(eps[:-l] * eps[l:]) - np.mean(eps) ** 2 if l else 1 - np.mean(eps) ** 2 for l in range(11)]
    assert np.allclose(c, np.array(direct) / c[0] * c[0], atol=2e-3)


def test_propagator_recovers_a_power_law_kernel():
    n, lmax = 400_000, 3000
    eps = long_memory_signs(n, seed=1)
    lag = np.arange(1, lmax + 1)
    G = 0.5 * lag ** -0.4
    rng = np.random.default_rng(2)
    # m_t = sum_{s<t} G(t - s) eps_s + noise
    m = np.convolve(eps, np.r_[0.0, G])[:n] + np.cumsum(rng.normal(0, 0.05, n))
    C = impact.sign_acf(eps, 300)
    R = impact.response(eps, m, 300)
    G_hat = impact.propagator(R, C, 300)
    assert np.max(np.abs(G_hat[:30] / G[:30] - 1)) < 0.15, G_hat[:10] / G[:10]


def test_hurst_of_brownian_motion_is_one_half():
    m = np.cumsum(np.random.default_rng(3).normal(size=200_000))
    assert abs(impact.hurst(m) - 0.5) < 0.03


def test_square_root_fit_recovers_the_exponent():
    rng = np.random.default_rng(4)
    q = np.exp(rng.uniform(np.log(1e-4), np.log(1e-1), 20000))
    imp = 0.8 * q ** 0.5 + rng.normal(0, 0.005, len(q))
    psi, lo, hi, Y = impact.fit_power(q, imp, n_boot=200)
    # Logs of noisy bin means bias psi slightly upwards (0.002 here), so the band need not cover 0.5.
    assert abs(psi - 0.5) < 0.01 and lo < psi < hi and hi - lo < 0.05 and abs(Y - 0.8) < 0.08, (psi, lo, hi, Y)


def test_sign_runs_group_consecutive_same_sign_trades():
    sign = np.array([1, 1, -1, -1, -1, 1, -1, -1])
    starts, ends = impact.sign_runs(sign, min_len=2)
    assert starts.tolist() == [0, 2, 6] and ends.tolist() == [1, 4, 7]


def test_optimal_schedule_beats_twap_and_is_symmetric_for_exponential_decay():
    n = 40
    G = np.exp(-0.2 * np.arange(n))
    x, cost, twap = impact.optimal_schedule(G, 1.0)
    assert np.isclose(x.sum(), 1.0) and cost <= twap + 1e-12
    assert np.allclose(x, x[::-1], atol=1e-9)
    assert x[0] > x[n // 2]  # Obizhaeva-Wang: larger trades at the ends


def test_kernel_positive_definiteness_check():
    assert impact.min_eigenvalue(np.exp(-0.2 * np.arange(50))) > 0
    bad = np.r_[1.0, 1.5, np.zeros(48)]  # increases: allows a profitable round trip
    assert impact.min_eigenvalue(bad) < 0


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
