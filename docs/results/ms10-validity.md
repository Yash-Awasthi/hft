# MS10: counterfactual validity

Validation day, INTC: 100 states sampled at one-tick spreads (L2 snapshot of 10 prices per side, the last 127 events, the 50 real events that followed). Interventions: market orders of 100, 300, 1,000, 3,000, 10,000 shares; outcome: mid change in ticks over 50 events; 64 paired seeds per state with shared random numbers. All subjects move the same L2 book with the same mechanics.

## Relations

| Relation | replay | queue_reactive | event_transformer |
|---|---|---|---|
| Stability (KS p, fresh seeds) | pass (1.00) | pass (0.28) | pass (1.00) |
| Side symmetry (max abs z) | pass (0.0) | pass (2.1) | FAIL (4.3) |
| Size monotonicity (min step z) | pass (2.2) | pass (1.1) | pass (3.0) |
| Concavity (psi in (0, 1)) | pass (0.68) | pass (0.40) | pass (0.58) |
| psi within the empirical band 0.05 [0.02, 0.08] | FAIL (0.68) | FAIL (0.40) | FAIL (0.58) |

Side symmetry: Delta(X, buy q) against -Delta(mirror X, sell q); fails if any size has |z| >= 3. Monotonicity fails if a larger order has a smaller mean effect by 2 standard errors. The empirical exponent comes from real market orders of the same sizes on the same day (sweeps merged), which carry information: levels are compared for consistency, not as causal ground truth.

## Mean effect by size (ticks)

| Shares | Empirical (n) | replay | queue_reactive | event_transformer |
|---|---|---|---|---|
| 100 | 0.626 (7,518) | 0.075 ± 0.028 | 0.089 ± 0.015 | 0.209 ± 0.017 |
| 300 | 0.732 (3,614) | 0.180 ± 0.038 | 0.163 ± 0.018 | 0.291 ± 0.021 |
| 1,000 | 0.756 (1,972) | 0.340 ± 0.048 | 0.325 ± 0.024 | 0.530 ± 0.031 |
| 3,000 | 0.752 (347) | 0.740 ± 0.074 | 0.480 ± 0.022 | 1.152 ± 0.047 |
| 10,000 | 0.844 (32) | 1.895 ± 0.164 | 0.514 ± 0.022 | 2.954 ± 0.107 |

Mechanical consistency: the transformer's generated cancels at empty prices are dropped; 5.98 per 50-event rollout.

## Not built

- M3 checkpoints (licence decision and download pending) and the Linna et al. forecaster method.
- Linear response, decay against R(l), scale collapse across stocks, path dependence; shrinking to a minimal counterexample beyond the smallest failing size; LOB-Bench realism scores; stress tests.
- The queue-reactive subject is model I without the Noble et al. kernel, and failed its MS7 validation.
- Nightly runs on the self-hosted runner (needs the runner).
