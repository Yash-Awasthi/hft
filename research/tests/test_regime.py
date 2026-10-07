"""Regime forecast checks: method A recovers a known cross-sectional curve and moves stocks
along it; method B follows the Dayri-Rosenbaum tick-change relation on hand cases."""

import pathlib
import sys

import numpy as np
import polars as pl

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import regime  # noqa: E402


def synthetic(n=600, seed=0):
    rng = np.random.default_rng(seed)
    x = rng.uniform(-3, 3, n)
    ln_notional, ln_mid = rng.normal(17, 1.5, n), rng.normal(3.5, 0.8, n)
    y = 0.8 * np.tanh(x) + 0.3 * ln_notional - 0.1 * ln_mid + rng.normal(0, 0.05, n)
    return pl.DataFrame({"x": x, "ln_notional": ln_notional, "ln_mid": ln_mid, "y": y}), y


def test_method_a_moves_stocks_along_the_fitted_curve():
    df, _ = synthetic()
    m = regime.fit_a(df, "y")
    d = regime.change_a(m, df, np.log(0.5))
    x = df["x"].to_numpy()
    want = 0.8 * (np.tanh(x + np.log(0.5)) - np.tanh(x))
    inside = np.abs(x) < 2.3  # away from the edges of the support
    assert np.max(np.abs(d[inside] - want[inside])) < 0.06, np.max(np.abs(d[inside] - want[inside]))


def test_leave_stocks_out_scores_a_good_fit_highly():
    df, _ = synthetic()
    r2, mae = regime.leave_stocks_out(df, "y", folds=5)
    assert r2 > 0.95 and mae < 0.06, (r2, mae)


def test_method_b_eta_and_spread():
    # eta = eta0 (alpha0 / alpha)^(1 - beta / 2); halving the tick with beta = 1 multiplies by sqrt 2.
    assert np.isclose(regime.eta_after(0.44, 0.5, 1.0), 0.44 * np.sqrt(2))
    assert np.isclose(regime.eta_after(0.44, 0.5, 0.5), 0.44 * 2 ** 0.75)
    # Tick-constrained stock: new spread is the larger of the new tick and 2 eta alpha.
    s = regime.spread_b(spread0=0.0104, eta0=0.44, tick0=0.01, factor=0.5, beta=1.0)
    assert np.isclose(s, 2 * 0.44 * np.sqrt(2) * 0.005)
    s = regime.spread_b(spread0=0.0101, eta0=0.2, tick0=0.01, factor=0.5, beta=1.0)
    assert np.isclose(s, 0.005)  # eta stays below 1/2: one new tick
    # Not tick-constrained: only the new tick binds.
    assert np.isclose(regime.spread_b(0.08, 0.7, 0.01, 0.5, 1.0), 0.08)
    assert np.isclose(regime.spread_b(0.03, 0.7, 0.01, 5.0, 1.0), 0.05)


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
