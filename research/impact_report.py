"""MS8 impact report from research/impact.py output: per group the sign memory, Hurst exponent,
propagator, square-root exponent from sign-run metaorders, the no-dynamic-arbitrage check of
the fitted kernel, and execution schedules compared under it.

Usage: python research/impact_report.py <impact.json> <out.md>
"""

import json
import pathlib
import sys

import numpy as np

import impact


def smooth_kernel(G, lags=(1, 2, 5, 10, 20, 50, 100, 200, 500)):
    """G at selected lags, for the table."""
    return [G[l - 1] for l in lags if l <= len(G)]


def power_fit(r):
    """Parametric kernel of a stock: stored by research/impact.py, else refitted from C and R."""
    return r.get("G_power") or impact.propagator_power_law(np.array(r["R"]), np.array(r["C"]), len(r["R"]) // 2)


def main():
    res = json.loads(pathlib.Path(sys.argv[1]).read_text())
    lines = ["# MS8: impact from public order-level data", "",
             "Train days 2025-12-08 to 2025-12-10, 09:35 to 15:55, universe stocks. Trades are executions "
             "merged per timestamp and sign; hidden prints are signed against the mid (Nasdaq reports every "
             "P message as a buy). Lags in trades.", ""]
    lines += ["## Per stock", "",
              "| Group | Stock | Trades | Mean sign | kappa | Hurst | G(1) bp of mid | G(500)/G(1) | Kernel min eig | Sign runs | psi [95%] |",
              "|---|---|---|---|---|---|---|---|---|---|---|"]
    by_group = {}
    for r in sorted(res, key=lambda r: (r["group"], r["symbol"])):
        G = np.array(r["G"])
        psi, lo, hi, _ = r["psi_runs"]
        lines.append(f"| {r['group']} | {r['symbol']} | {r['trades']:,} | {r['mean_sign']:+.3f} | {r['kappa']:.2f} | "
                     f"{r['hurst']:.3f} | {G[0] * 1e4:.3f} | {G[-1] / G[0]:.2f} | {r['kernel_min_eig']:.4f} | "
                     f"{r['sign_runs']:,} | {psi:.2f} [{lo:.2f}, {hi:.2f}] |")
        by_group.setdefault(r["group"], []).append(r)
    lines += ["", "G is in dollars per trade sign; the column divides by nothing, so compare within a stock.", ""]

    lines += ["## By group", "",
              "| Group | Stocks | kappa median | Hurst median | psi median | psi IQR | Unregularized kernels failing the check | Attributed runs |",
              "|---|---|---|---|---|---|---|---|"]
    for g, rs in sorted(by_group.items()):
        psis = np.array([r["psi_runs"][0] for r in rs])
        lines.append(f"| {g} | {len(rs)} | {np.median([r['kappa'] for r in rs]):.2f} | "
                     f"{np.median([r['hurst'] for r in rs]):.3f} | {np.nanmedian(psis):.2f} | "
                     f"{np.nanquantile(psis, 0.25):.2f} to {np.nanquantile(psis, 0.75):.2f} | "
                     f"{sum(r['kernel_min_eig'] < 0 for r in rs)} of {len(rs)} | "
                     f"{sum(r['attributed_runs'] for r in rs)} |")

    fits = {r["symbol"]: power_fit(r) for r in res}
    lines += ["", "## Parametric kernel", "",
              "G(l) = G0 (1 + l / l0)^-beta fitted to R(1..250) by bounded least squares (G0 >= 0, beta in "
              "[0, 3], l0 in [0.1, 10^4]), the kernel summed over all 500 lags of C. G0 >= 0 and beta >= 0 make G "
              "positive, decreasing and convex, so the Toeplitz matrix is positive semi-definite (Polya) by "
              "construction; the minimum eigenvalue over lags 0..500 is reported as a check. Fit error: RMSE of R over mean |R|.", "",
              "| Group | Stock | G0 bp of mid | l0 | beta | Fit error | Min eig | At a bound |", "|---|---|---|---|---|---|---|---|"]
    for r in sorted(res, key=lambda r: (r["group"], r["symbol"])):
        p = fits[r["symbol"]]
        r["power_min_eig"] = impact.min_eigenvalue(impact.power_law_kernel(p, len(r["G"])))
        r["power_err"] = p["rmse"] / float(np.mean(np.abs(r["R"][:len(r["R"]) // 2])))
        r["power_bound"] = p["beta"] >= 3 - 1e-6 or p["l0"] <= 0.1 + 1e-6 or p["l0"] >= 1e4 * (1 - 1e-6) or p["beta"] <= 1e-6
        lines.append(f"| {r['group']} | {r['symbol']} | {p['G0'] * 1e4:.3f} | {p['l0']:.2f} | {p['beta']:.3f} | "
                     f"{r['power_err']:.3f} | {r['power_min_eig']:.2e} | {'yes' if r['power_bound'] else 'no'} |")
    lines += ["", "| Group | Stocks | Parametric kernels failing the check | beta median | Fit error median | At a bound |",
              "|---|---|---|---|---|---|"]
    for g, rs in sorted(by_group.items()):
        lines.append(f"| {g} | {len(rs)} | {sum(r['power_min_eig'] < -1e-12 for r in rs)} of {len(rs)} | "
                     f"{np.median([fits[r['symbol']]['beta'] for r in rs]):.3f} | "
                     f"{np.median([r['power_err'] for r in rs]):.3f} | {sum(r['power_bound'] for r in rs)} |")

    lines += ["", "## Execution schedules under the parametric kernel", "",
              "Cost of executing X over n equal slots, x' Gamma x / 2 with Gamma_ij = G(|i - j|), relative to "
              "TWAP, with the group's median l0 and beta (G0 cancels in the ratio).", "",
              "| Group | l0 | beta | n | Optimal / TWAP cost | Front-loading x1 / (X/n) |", "|---|---|---|---|---|---|"]
    for g, rs in sorted(by_group.items()):
        p = {"G0": 1.0, "l0": float(np.median([fits[r["symbol"]]["l0"] for r in rs])),
             "beta": float(np.median([fits[r["symbol"]]["beta"] for r in rs]))}
        for n in (10, 50):
            x, cost, twap = impact.optimal_schedule(impact.power_law_kernel(p, n - 1), 1.0)
            lines.append(f"| {g} | {p['l0']:.2f} | {p['beta']:.3f} | {n} | {cost / twap:.3f} | {x[0] * n:.2f} |")
    lines += ["", "## Not built", "",
              "- Mechanical versus reactive split: needs the simulator with reaction on and off; the queue-reactive "
              "simulator failed validation at the current tick (MS7), so the split is not reported.",
              "- Synthetic metaorders of Maitrier, Loeper and Bouchaud, injected metaorders, the latent order book "
              "sweeps, Almgren-Chriss and VWAP comparisons in replay, and the half-penny prediction of psi.",
              "- Attributed metaorders: attributed executions are about 0.01% of executions on these stocks, too few "
              "to fit.", ""]
    pathlib.Path(sys.argv[2]).write_text("\n".join(lines))


if __name__ == "__main__":
    main()
