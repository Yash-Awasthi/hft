"""Univariate Hawkes process with an exponential kernel,
lambda(t) = mu + sum_{t_i < t} alpha * beta * exp(-beta (t - t_i)),
so alpha is the branching ratio (expected children per event). Maximum likelihood with the
O(n) recursion of Ozaki (1979); simulation by Ogata thinning for parameter recovery.
"""

import numpy as np
from scipy.optimize import minimize


def loglik(params, t, horizon):
    mu, alpha, beta = params
    if mu <= 0 or alpha < 0 or alpha >= 1 or beta <= 0:
        return -np.inf
    a = 0.0  # sum over earlier events of exp(-beta (t_i - t_j))
    ll = 0.0
    prev = None
    for ti in t:
        if prev is not None:
            a = np.exp(-beta * (ti - prev)) * (1 + a)
        ll += np.log(mu + alpha * beta * a)
        prev = ti
    compensator = mu * horizon + alpha * np.sum(1 - np.exp(-beta * (horizon - t)))
    return ll - compensator


def _loglik_fast(params, t, horizon):
    mu, alpha, beta = params
    dt = np.diff(t)
    decay = np.exp(-beta * dt)
    # a_i = decay_i * (1 + a_{i-1}); solved as a linear recursion with cumulative products.
    a = np.zeros(len(t))
    for i in range(1, len(t)):
        a[i] = decay[i - 1] * (1 + a[i - 1])
    ll = np.sum(np.log(mu + alpha * beta * a))
    return ll - mu * horizon - alpha * np.sum(1 - np.exp(-beta * (horizon - t)))


def fit(t, horizon, start=(None, 0.5, 10.0)):
    """MLE of (mu, alpha, beta) for event times t in [0, horizon], seconds."""
    t = np.asarray(t, dtype=float)
    mu0 = start[0] or max(len(t) / horizon * (1 - start[1]), 1e-6)
    x0 = np.log([mu0, start[1] / (1 - start[1]), start[2]])

    def nll(x):
        mu, r, beta = np.exp(x)
        alpha = r / (1 + r)
        v = _loglik_fast((mu, alpha, beta), t, horizon)
        return -v if np.isfinite(v) else 1e300

    res = minimize(nll, x0, method="Nelder-Mead", options={"xatol": 1e-6, "fatol": 1e-6, "maxiter": 4000})
    mu, r, beta = np.exp(res.x)
    return mu, r / (1 + r), beta


def simulate(mu, alpha, beta, horizon, rng):
    """Ogata thinning."""
    t, out, excite = 0.0, [], 0.0  # excite = sum alpha*beta*exp(-beta (t - t_i))
    while True:
        lam_bar = mu + excite
        w = rng.exponential(1 / lam_bar)
        t += w
        if t >= horizon:
            return np.array(out)
        excite *= np.exp(-beta * w)
        if rng.uniform() * lam_bar <= mu + excite:
            out.append(t)
            excite += alpha * beta
