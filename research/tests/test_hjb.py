"""Avellaneda-Stoikov / Gueant-Lehalle-Fernandez-Tapia checks: the numerical HJB solution
against the exact matrix-exponential solution, and the closed forms used by the C++ baseline
against both."""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import hjb  # noqa: E402

P = dict(A=1.0, k=1.5, gamma=0.05, sigma2=0.05, Q=5)


def test_hamiltonian_by_golden_section_matches_first_order_condition():
    p = np.linspace(-2, 2, 9)
    h_num = hjb.hamiltonian(p, P["A"], P["k"], P["gamma"])
    d = p + np.log1p(P["gamma"] / P["k"]) / P["gamma"]
    h_foc = P["A"] * np.exp(-P["k"] * d) / (P["k"] + P["gamma"])
    assert np.allclose(h_num, h_foc, rtol=1e-9, atol=1e-12)


def test_numerical_hjb_matches_exact_solution():
    T = 60.0
    th_num = hjb.solve_numerical(T=T, steps=6000, **P)
    th_ex = hjb.solve_exact(T=T, **P)
    assert np.max(np.abs(th_num - th_ex)) < 1e-8, np.max(np.abs(th_num - th_ex))
    b_num, a_num = hjb.quotes(th_num, P["k"], P["gamma"])
    b_ex, a_ex = hjb.quotes(th_ex, P["k"], P["gamma"])
    assert np.nanmax(np.abs(b_num - b_ex)) < 1e-8 and np.nanmax(np.abs(a_num - a_ex)) < 1e-8


def test_quotes_skew_against_inventory_and_are_symmetric():
    b, a = hjb.quotes(hjb.solve_exact(T=600.0, **P), P["k"], P["gamma"])
    q = np.arange(-P["Q"], P["Q"] + 1)
    inner = (q > -P["Q"]) & (q < P["Q"])
    assert np.all(np.diff(b[:-1]) > 0) and np.all(np.diff(a[1:]) < 0)
    assert np.allclose(b[inner], a[inner][::-1])


def test_asymptotic_closed_form_is_close_near_zero_inventory_for_long_horizons():
    th = hjb.solve_exact(T=3600.0, **P)
    b, a = hjb.quotes(th, P["k"], P["gamma"])
    b_cf, a_cf = hjb.glft_asymptotic(np.arange(-P["Q"], P["Q"] + 1), P["A"], P["k"], P["gamma"], P["sigma2"])
    mid = P["Q"]
    assert abs(b[mid] - b_cf[mid]) < 0.05 and abs(a[mid] - a_cf[mid]) < 0.05, (b[mid], b_cf[mid])


def test_as_closed_form_converges_as_horizon_shrinks():
    q = np.arange(-P["Q"], P["Q"] + 1)
    errs = []
    inner = np.abs(q) <= 2  # away from the inventory bounds, which AS does not model
    for T in (10.0, 1.0, 0.1):
        b, _ = hjb.quotes(hjb.solve_exact(T=T, **P), P["k"], P["gamma"])
        b_as, _ = hjb.as_closed_form(q, T, P["k"], P["gamma"], P["sigma2"])
        errs.append(np.max(np.abs(b - b_as)[inner]))
    assert errs[0] > 10 * errs[1] > 100 * errs[2], errs


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
