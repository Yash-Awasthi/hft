"""Statistics for results (DESIGN.md sections 1, 4): bootstrap intervals clustered by day,
Sharpe ratios on intraday buckets, the probabilistic and deflated Sharpe ratios of Bailey and
Lopez de Prado (2012, 2014), and the CRPS of a sample forecast."""

import numpy as np
from scipy.stats import kurtosis, norm, skew

EULER_GAMMA = 0.5772156649015329


def cluster_bootstrap(values, clusters, stat=np.mean, n=4000, alpha=0.05, seed=0):
    """Percentile interval of stat(values), resampling whole clusters with replacement."""
    values, clusters = np.asarray(values, float), np.asarray(clusters)
    ids = np.unique(clusters)
    groups = [values[clusters == c] for c in ids]
    rng = np.random.default_rng(seed)
    draws = np.empty(n)
    for i in range(n):
        pick = rng.integers(0, len(groups), len(groups))
        draws[i] = stat(np.concatenate([groups[j] for j in pick]))
    lo, hi = np.quantile(draws, [alpha / 2, 1 - alpha / 2])
    return float(stat(values)), float(lo), float(hi)


def sharpe(returns):
    r = np.asarray(returns, float)
    return float(np.mean(r) / np.std(r, ddof=1)) if len(r) > 1 and np.std(r) > 0 else float("nan")


def probabilistic_sharpe(returns, sr_benchmark=0.0):
    """P(true Sharpe > benchmark) given T observations, skewness and kurtosis (PSR)."""
    r = np.asarray(returns, float)
    sr, t = sharpe(r), len(r)
    g3, g4 = skew(r), kurtosis(r, fisher=False)
    denom = np.sqrt(1 - g3 * sr + (g4 - 1) / 4 * sr ** 2)
    return float(norm.cdf((sr - sr_benchmark) * np.sqrt(t - 1) / denom))


def expected_max_sharpe(n_trials, var_sr):
    """Expected maximum Sharpe of n_trials unskilled strategies with Sharpe variance var_sr."""
    if n_trials <= 1:
        return 0.0
    return float(np.sqrt(var_sr) * ((1 - EULER_GAMMA) * norm.ppf(1 - 1 / n_trials)
                                    + EULER_GAMMA * norm.ppf(1 - 1 / (n_trials * np.e))))


def deflated_sharpe(returns, n_trials, var_sr):
    """PSR against the Sharpe the best of n_trials unskilled trials would show (DSR)."""
    return probabilistic_sharpe(returns, expected_max_sharpe(n_trials, var_sr))


def crps_sample(samples, observed):
    """CRPS of a forecast given as samples: E|X - y| - E|X - X'| / 2."""
    x = np.sort(np.asarray(samples, float))
    n = len(x)
    term1 = np.mean(np.abs(x - observed))
    # E|X - X'| from the sorted sample in O(n).
    term2 = 2 * np.sum(x * (2 * np.arange(1, n + 1) - n - 1)) / (n * n)
    return float(term1 - term2 / 2)
