"""MS10 report: pass/fail matrix of subjects against the counterfactual relations.

Usage: python research/validity_report.py <validity.json> <out.md>
"""

import json
import pathlib
import sys

import numpy as np


def main():
    r = json.loads(pathlib.Path(sys.argv[1]).read_text())
    qs = r["QS"]
    emp = r["empirical"]
    S = r["subjects"]
    em = [e["mean"] for e in emp]
    ok = lambda b: "pass" if b else "FAIL"
    lines = [
        "# MS10: counterfactual validity", "",
        f"Validation day, {r['symbol']}: {r['states']} states sampled at one-tick spreads (L2 snapshot of 10 "
        f"prices per side, the last 127 events, the {r['h']} real events that followed). Interventions: market "
        f"orders of {', '.join(f'{q:,}' for q in qs)} shares; outcome: mid change in ticks over {r['h']} events; "
        f"{r['seeds']} paired seeds per state with shared random numbers. All subjects move the same L2 book with "
        "the same mechanics.", "",
        "## Relations", "",
        "| Relation | " + " | ".join(S) + " |", "|---|" + "---|" * len(S),
        "| Stability (KS p, fresh seeds) | " + " | ".join(f"{ok(v['stability_pass'])} ({v['stability_ks_p']:.2f})" for v in S.values()) + " |",
        "| Side symmetry (max abs z) | " + " | ".join(f"{ok(v['side_symmetry_pass'])} ({np.nanmax(np.abs(v['side_symmetry_z'])):.1f})" for v in S.values()) + " |",
        "| Size monotonicity (min step z) | " + " | ".join(f"{ok(v['monotonic_pass'])} ({min(v['monotonic_z']):.1f})" for v in S.values()) + " |",
        "| Concavity (psi in (0, 1)) | " + " | ".join(f"{ok(v['concavity_pass'])} ({v['psi']:.2f})" for v in S.values()) + " |",
        "| psi within the empirical band {:.2f} [{:.2f}, {:.2f}] | ".format(S[next(iter(S))]["psi_empirical"], *S[next(iter(S))]["psi_empirical_band"])
        + " | ".join(f"{ok(v['psi_within_empirical_band'])} ({v['psi']:.2f})" for v in S.values()) + " |",
        "", "Side symmetry: Delta(X, buy q) against -Delta(mirror X, sell q); fails if any size has |z| >= 3. "
        "Monotonicity fails if a larger order has a smaller mean effect by 2 standard errors. The empirical "
        "exponent comes from real market orders of the same sizes on the same day (sweeps merged), which carry "
        "information: levels are compared for consistency, not as causal ground truth.", "",
        "## Mean effect by size (ticks)", "",
        "| Shares | Empirical (n) | " + " | ".join(S) + " |", "|---|---|" + "---|" * len(S),
    ]
    for k, q in enumerate(qs):
        e = emp[k]
        lines.append(f"| {q:,} | {e['mean']:.3f} ({e['n']:,}) | " if e["mean"] is not None else f"| {q:,} | - | ")
        lines[-1] += " | ".join(f"{v['mean_effect'][k]:.3f} ± {v['se'][k]:.3f}" for v in S.values()) + " |"
    if "event_transformer" in S and "invalid_ops_per_rollout" in S["event_transformer"]:
        lines += ["", f"Mechanical consistency: the transformer's generated cancels at empty prices are dropped; "
                  f"{S['event_transformer']['invalid_ops_per_rollout']:.2f} per {r['h']}-event rollout."]
    lines += ["", "## Not built", "",
              "- M3 checkpoints (licence decision and download pending) and the Linna et al. forecaster method.",
              "- Linear response, decay against R(l), scale collapse across stocks, path dependence; shrinking to a "
              "minimal counterexample beyond the smallest failing size; LOB-Bench realism scores; stress tests.",
              "- The queue-reactive subject is model I without the Noble et al. kernel, and failed its MS7 validation.",
              "- Nightly runs on the self-hosted runner (needs the runner).", ""]
    pathlib.Path(sys.argv[2]).write_text("\n".join(lines))


if __name__ == "__main__":
    main()
