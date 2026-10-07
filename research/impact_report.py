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
              "| Group | Stocks | kappa median | Hurst median | psi median | psi IQR | Kernels failing the check | Attributed runs |",
              "|---|---|---|---|---|---|---|---|"]
    for g, rs in sorted(by_group.items()):
        psis = np.array([r["psi_runs"][0] for r in rs])
        lines.append(f"| {g} | {len(rs)} | {np.median([r['kappa'] for r in rs]):.2f} | "
                     f"{np.median([r['hurst'] for r in rs]):.3f} | {np.nanmedian(psis):.2f} | "
                     f"{np.nanquantile(psis, 0.25):.2f} to {np.nanquantile(psis, 0.75):.2f} | "
                     f"{sum(r['kernel_min_eig'] < 0 for r in rs)} of {len(rs)} | "
                     f"{sum(r['attributed_runs'] for r in rs)} |")

    # Execution under a kernel: the group median of G normalized to G(1), and a decaying
    # power-law fit to it so the comparison obeys the no-arbitrage condition.
    lines += ["", "## Execution schedules under the fitted kernel", "",
              "Cost of executing X over n equal slots, x' Gamma x / 2 with Gamma_ij = G(|i - j|), relative to "
              "TWAP. The empirical kernels do not decay (above), so a decaying power law G(l) = G(1) l^-beta is "
              "fitted to lags 1 to 50 of each group's median kernel and used here.", "",
              "| Group | beta | n | Optimal / TWAP cost | Front-loading x1 / (X/n) |", "|---|---|---|---|---|"]
    for g, rs in sorted(by_group.items()):
        Gm = np.median([np.array(r["G"]) / r["G"][0] for r in rs], axis=0)
        lag = np.arange(1, 51)
        beta = float(-np.polyfit(np.log(lag), np.log(np.maximum(Gm[:50], 1e-9)), 1)[0])
        for n in (10, 50):
            ker = np.r_[1.0, np.arange(1, n) ** -max(beta, 0.05)]
            x, cost, twap = impact.optimal_schedule(ker, 1.0)
            lines.append(f"| {g} | {beta:.2f} | {n} | {cost / twap:.3f} | {x[0] * n:.2f} |")
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
