"""Queue-reactive parameter recovery (DESIGN.md MS7): simulate with known rates through the C++
source, ingest the ITCH stream into a store, record events with the same recorder used on real
data, and recover the rates and theta within their Poisson standard errors.

Usage: python research/tests/test_qr.py <py-build> <ingest-binary>
"""

import pathlib
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import qr  # noqa: E402

K, N, AES = 3, 25, 100
START = 34_200_000_000_000
END = START + 6 * 3600 * 1_000_000_000


def truth():
    n = np.arange(N + 1)
    L = np.vstack([np.full(N + 1, 1.0 / (1 + i)) * (1 + 0.5 * np.exp(-n / 3)) for i in range(K)])
    C = np.vstack([0.3 * n * (1 + 0.2 * i) for i in range(K)])
    M = np.vstack([np.full(N + 1, 0.8 if i == 0 else 0.4) for i in range(K)])
    init = np.vstack([np.where((n >= 2) & (n <= 8), 1.0, 0.0) for _ in range(K)])
    return L, C, M, init / init.sum(axis=1, keepdims=True), 0.55


def test_recovers_rates_and_theta(py_build, ingest):
    sys.path.insert(0, py_build)
    import hftpy

    L, C, M, init, theta = truth()
    with tempfile.TemporaryDirectory() as d:
        raw, store = pathlib.Path(d) / "sim.itch", pathlib.Path(d) / "store"
        info = hftpy.qr_simulate(K, N, AES, 200_050, theta, L.ravel().tolist(), C.ravel().tolist(),
                                 M.ravel().tolist(), init.ravel().tolist(), START, END, 21, str(raw))
        store.mkdir()
        with open(raw, "rb") as f:
            subprocess.run([ingest, str(store)], stdin=f, check=True, capture_output=True)
        loc = hftpy.symbols(str(store))["QRSIM"]
        ev = hftpy.qr_events(str(store), loc, K, 100, START, END)
    fit = qr.calibrate(ev, K, N, tick=100)
    assert fit["aes"] == AES
    twice = qr.pool([fit, fit])
    seen = fit["n_L"] > 0
    assert np.allclose(twice["L"], fit["L"]) and np.allclose(twice["L_se"][seen] * np.sqrt(2), fit["L_se"][seen])
    assert twice["theta"] == fit["theta"]
    assert ev["episodes_moved"] == info["moves"]
    z_all = []
    for name, true in (("L", L), ("C", C), ("M", M)):
        est, se, t = fit[name], fit[name + "_se"], fit["time_" + name]
        ok = (t > 50) & (se > 0)  # seconds in the state
        if name != "L":
            ok[:, 0] = False  # nothing to cancel or execute
        assert (ok.sum(axis=1) >= 3).all(), (name, ok.sum(axis=1))
        z = (est[ok] - true[ok]) / se[ok]
        z_all.append(z)
        assert np.max(np.abs(z)) < 5, (name, np.max(np.abs(z)))
    z = np.concatenate(z_all)
    assert abs(z.mean()) < 0.3 and 0.7 < z.std() < 1.3, (z.mean(), z.std())
    assert abs(fit["theta"] - theta) < 4 * fit["theta_se"], (fit["theta"], fit["theta_se"])


def test_half_tick_model_runs_on_the_half_penny_grid_and_is_recovered(py_build, ingest):
    sys.path.insert(0, py_build)
    import hftpy

    L, C, M, init, theta = truth()
    fit = {"K": K, "N": N, "aes": AES, "tick": 100, "theta": theta, "L": L, "C": C, "M": M, "init": init,
           "time_L": np.ones_like(L), "time_C": np.ones_like(L), "time_M": np.ones_like(L)}
    half = qr.half_tick(fit, inner_share=2 / 3)
    assert half["K"] == 2 * K and half["tick"] == 50
    assert np.allclose(half["L"][0::2] + half["L"][1::2], L)
    assert np.allclose(half["init"].sum(axis=1), 1)
    with tempfile.TemporaryDirectory() as d:
        raw, store = pathlib.Path(d) / "sim.itch", pathlib.Path(d) / "store"
        hftpy.qr_simulate(**qr.simulate_args(half, 200_025, START, END, 4, raw))
        store.mkdir()
        with open(raw, "rb") as f:
            subprocess.run([ingest, str(store)], stdin=f, check=True, capture_output=True)
        loc = hftpy.symbols(str(store))["QRSIM"]
        ev = hftpy.qr_events(str(store), loc, 2 * K, 50, START, END)
    back = qr.calibrate(ev, 2 * K, N, tick=50)
    ok = back["time_L"] > 50
    z = (back["L"][ok] - half["L"][ok]) / back["L_se"][ok]
    assert np.max(np.abs(z)) < 5, np.max(np.abs(z))
    assert abs(back["theta"] - theta) < 4 * back["theta_se"], (back["theta"], back["theta_se"])


if __name__ == "__main__":
    test_recovers_rates_and_theta(sys.argv[1], sys.argv[2])
    print("test_recovers_rates_and_theta ok")
    test_half_tick_model_runs_on_the_half_penny_grid_and_is_recovered(sys.argv[1], sys.argv[2])
    print("test_half_tick_model_runs_on_the_half_penny_grid_and_is_recovered ok")
