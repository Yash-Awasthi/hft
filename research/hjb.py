"""Avellaneda-Stoikov market making with bounded inventory (Gueant, Lehalle and
Fernandez-Tapia 2013), prices in ticks, time in seconds, inventory in lots.

With u = -exp(-gamma (x + q s + theta(t, q))) the HJB reduces to
  theta_t - gamma sigma^2 q^2 / 2 + H(theta(q) - theta(q+1)) 1{q<Q} + H(theta(q) - theta(q-1)) 1{q>-Q} = 0,
  H(p) = sup_d A exp(-k d) (1 - exp(-gamma (d - p))) / gamma,  theta(T, q) = 0,
and the optimal bid distance is the maximizer d* = p + ln(1 + gamma/k) / gamma at p = theta(q) - theta(q+1).
solve_numerical maximizes H by golden section and integrates backward with RK4;
solve_exact uses the linear ODE of w = exp(k theta): w(0) = exp(-M T) 1, M symmetric.

Usage: python research/hjb.py <out.json>   (table for the MS6 report)
"""

import json
import sys

import numpy as np

GOLD = (np.sqrt(5) - 1) / 2


def hamiltonian(p, A, k, gamma, iters=80):
    """sup over d of A e^{-k d} (1 - e^{-gamma (d - p)}) / gamma, by golden section on d."""
    p = np.asarray(p, float)
    f = lambda d: A * np.exp(-k * d) * (-np.expm1(-gamma * (d - p))) / gamma
    lo, hi = p.copy(), p + 50.0 / k  # f < 0 below p and decays to 0 far above
    for _ in range(iters):
        x1, x2 = hi - GOLD * (hi - lo), lo + GOLD * (hi - lo)
        left = f(x1) > f(x2)
        hi, lo = np.where(left, x2, hi), np.where(left, lo, x1)
    return f((lo + hi) / 2)


def _rhs(theta, A, k, gamma, sigma2):
    """-theta_t: the HJB terms other than the time derivative."""
    q = np.arange(len(theta)) - (len(theta) - 1) // 2
    out = -0.5 * gamma * sigma2 * q ** 2
    out[:-1] += hamiltonian(theta[:-1] - theta[1:], A, k, gamma)
    out[1:] += hamiltonian(theta[1:] - theta[:-1], A, k, gamma)
    return out


def solve_numerical(A, k, gamma, sigma2, Q, T, steps):
    """theta(0, q) for q = -Q..Q by RK4 backward from theta(T) = 0."""
    th = np.zeros(2 * Q + 1)
    h = T / steps
    f = lambda x: _rhs(x, A, k, gamma, sigma2)
    for _ in range(steps):
        k1 = f(th); k2 = f(th + h / 2 * k1); k3 = f(th + h / 2 * k2); k4 = f(th + h * k3)
        th = th + h / 6 * (k1 + 2 * k2 + 2 * k3 + k4)
    return th


def solve_exact(A, k, gamma, sigma2, Q, T):
    q = np.arange(-Q, Q + 1)
    alpha = k * gamma * sigma2 / 2
    eta = A * (1 + gamma / k) ** (-(1 + k / gamma))
    M = np.diag(alpha * q ** 2.0) - eta * (np.eye(len(q), k=1) + np.eye(len(q), k=-1))
    lam, V = np.linalg.eigh(M)
    # Shifted by the smallest eigenvalue so long horizons do not underflow; quotes use differences only.
    w = V @ (np.exp(-(lam - lam[0]) * T) * (V.T @ np.ones(len(q))))
    return (np.log(w) - lam[0] * T) / k


def quotes(theta, k, gamma):
    """Optimal bid and ask distances from the mid per inventory; NaN where the side is off."""
    base = np.log1p(gamma / k) / gamma
    bid, ask = np.full(len(theta), np.nan), np.full(len(theta), np.nan)
    bid[:-1] = theta[:-1] - theta[1:] + base
    ask[1:] = theta[1:] - theta[:-1] + base
    return bid, ask


def glft_asymptotic(q, A, k, gamma, sigma2):
    """The closed form served by the C++ baseline (strategies.hpp, glft = true)."""
    base = np.log1p(gamma / k) / gamma
    c = np.sqrt(sigma2 * gamma / (2 * k * A) * (1 + gamma / k) ** (1 + k / gamma))
    return base + (2 * q + 1) / 2 * c, base - (2 * q - 1) / 2 * c


def as_closed_form(q, tau, k, gamma, sigma2):
    """Avellaneda-Stoikov (2008) approximation with time to close tau (glft = false)."""
    half = np.log1p(gamma / k) / gamma + gamma * sigma2 * tau / 2
    r_off = -q * gamma * sigma2 * tau
    return half - r_off, half + r_off


def report(A=1.0, k=1.5, gamma=0.05, sigma2=0.05, Q=5):
    q = np.arange(-Q, Q + 1)
    rows = []
    for T in (1.0, 60.0, 600.0, 3600.0, 23400.0):
        th_n = solve_numerical(A, k, gamma, sigma2, Q, T, steps=max(2000, int(T)))
        th_e = solve_exact(A, k, gamma, sigma2, Q, T)
        b_n, _ = quotes(th_n, k, gamma)
        b_e, _ = quotes(th_e, k, gamma)
        b_g, _ = glft_asymptotic(q, A, k, gamma, sigma2)
        b_a, _ = as_closed_form(q, T, k, gamma, sigma2)
        inner = slice(0, -1)
        rows.append((T, np.nanmax(np.abs(b_n - b_e)), b_e[Q], b_g[Q], b_a[Q],
                     np.max(np.abs(b_g - b_e)[inner]), np.max(np.abs(b_a - b_e)[inner])))
    head = ("| Horizon s | Numerical vs exact (max, ticks) | Exact bid q=0 | GLFT asymptotic q=0 | "
            "AS q=0 | GLFT max error | AS max error |\n|---|---|---|---|---|---|---|")
    body = "\n".join(f"| {T:,.0f} | {e:.1e} | {be:.3f} | {bg:.3f} | {ba:.3f} | {eg:.3f} | {ea:.3f} |"
                     for T, e, be, bg, ba, eg, ea in rows)
    text = (f"Parameters of the baseline: A = {A}/s, k = {k}/tick, gamma = {gamma}, sigma^2 = {sigma2} "
            f"ticks^2/s, inventory -{Q}..{Q} lots. Bid distance from the mid in ticks; errors are the "
            f"maximum over inventories that quote a bid. The numerical solution maximizes the "
            f"Hamiltonian by golden section and integrates the HJB backward with RK4.\n\n" + head + "\n" + body)
    return {"rows": [list(map(float, r)) for r in rows], "text": text}


if __name__ == "__main__":
    r = report()
    open(sys.argv[1], "w").write(json.dumps(r, indent=1))
    print(r["text"])
