# MS8: impact from public order-level data

Train days 2025-12-08 to 2025-12-10, 09:35 to 15:55, universe stocks. Trades are executions merged per timestamp and sign; hidden prints are signed against the mid (Nasdaq reports every P message as a buy). Lags in trades.

## Per stock

| Group | Stock | Trades | Mean sign | kappa | Hurst | G(1) bp of mid | G(500)/G(1) | Kernel min eig | Sign runs | psi [95%] |
|---|---|---|---|---|---|---|---|---|---|---|
| large_tick | AAL | 9,404 | -0.009 | 0.53 | 0.478 | 29.259 | 1.75 | -0.0089 | 1,706 | 0.43 [0.39, 0.47] |
| large_tick | BAC | 28,716 | -0.032 | -0.02 | 0.501 | 36.389 | 1.69 | -0.0113 | 5,723 | 0.45 [0.41, 0.47] |
| large_tick | BKR | 28,473 | +0.054 | 0.08 | 0.490 | 53.109 | 1.06 | -0.0121 | 5,900 | 0.50 [0.47, 0.52] |
| large_tick | CARR | 31,894 | +0.035 | 0.17 | 0.459 | 55.665 | 1.47 | -0.0391 | 7,414 | 0.41 [0.39, 0.43] |
| large_tick | CFLT | 12,438 | -0.048 | 0.65 | 0.442 | 7.717 | 3.07 | -0.0089 | 1,398 | 0.33 [0.29, 0.37] |
| large_tick | CIFR | 48,387 | -0.070 | 0.26 | 0.499 | 58.634 | 1.10 | -0.0047 | 10,924 | 0.41 [0.40, 0.43] |
| large_tick | CMCSA | 14,038 | -0.032 | 0.20 | 0.500 | 26.782 | 2.70 | -0.0404 | 2,620 | 0.53 [0.50, 0.59] |
| large_tick | CSCO | 34,873 | +0.047 | 0.11 | 0.499 | 44.224 | 1.76 | -0.0377 | 7,592 | 0.51 [0.48, 0.53] |
| large_tick | EXAS | 8,128 | -0.016 | 0.46 | 0.458 | 29.721 | 1.66 | -0.0076 | 1,514 | 0.36 [0.32, 0.40] |
| large_tick | GM | 24,224 | +0.003 | -0.02 | 0.497 | 59.816 | 2.05 | -0.1004 | 5,341 | 0.44 [0.42, 0.47] |
| large_tick | INTC | 62,008 | +0.031 | 0.32 | 0.503 | 40.964 | 1.69 | -0.0054 | 12,351 | 0.46 [0.45, 0.48] |
| large_tick | MDLZ | 27,640 | +0.025 | 0.25 | 0.446 | 49.809 | 1.14 | -0.0208 | 6,074 | 0.46 [0.44, 0.48] |
| large_tick | NFLX | 255,991 | -0.004 | 0.27 | 0.497 | 37.624 | 1.76 | -0.0033 | 50,381 | 0.42 [0.42, 0.43] |
| large_tick | NVDA | 405,722 | -0.024 | 0.65 | 0.491 | 51.985 | 1.36 | -0.0021 | 87,755 | 0.48 [0.47, 0.48] |
| large_tick | ONDS | 26,028 | -0.005 | 0.43 | 0.487 | 46.097 | 1.58 | -0.0050 | 5,587 | 0.39 [0.38, 0.41] |
| large_tick | PYPL | 35,111 | -0.043 | -0.31 | 0.505 | 53.164 | 1.45 | -0.0428 | 7,531 | 0.45 [0.44, 0.47] |
| large_tick | RGTI | 79,084 | -0.105 | 0.64 | 0.505 | 54.371 | 1.36 | -0.0023 | 17,156 | 0.48 [0.47, 0.50] |
| large_tick | RIVN | 23,570 | -0.034 | 0.18 | 0.488 | 40.363 | 2.14 | -0.0403 | 5,138 | 0.46 [0.44, 0.49] |
| large_tick | SMCI | 43,054 | -0.001 | 0.48 | 0.490 | 50.963 | 1.13 | -0.0074 | 9,739 | 0.44 [0.42, 0.46] |
| large_tick | SOFI | 42,906 | -0.011 | 0.92 | 0.479 | 34.195 | 1.28 | -0.0018 | 8,457 | 0.43 [0.41, 0.44] |
| large_tick | VZ | 12,184 | -0.052 | 0.47 | 0.502 | 38.638 | 2.80 | -0.0926 | 2,393 | 0.49 [0.45, 0.52] |
| large_tick | WBD | 40,243 | +0.052 | 0.58 | 0.499 | 29.215 | 1.89 | -0.0032 | 7,279 | 0.51 [0.49, 0.53] |
| large_tick | WFC | 45,882 | -0.047 | 0.48 | 0.480 | 59.128 | 1.62 | -0.0172 | 10,606 | 0.43 [0.40, 0.45] |
| large_tick | WMT | 55,256 | +0.004 | 0.32 | 0.494 | 58.655 | 2.10 | -0.1572 | 12,261 | 0.46 [0.44, 0.48] |
| large_tick | XOM | 56,456 | -0.014 | 0.34 | 0.504 | 56.871 | 1.68 | -0.0083 | 12,055 | 0.47 [0.46, 0.50] |
| small_tick | ADBE | 55,312 | -0.019 | 0.79 | 0.507 | 143.770 | 2.40 | -0.0326 | 8,611 | 0.43 [0.41, 0.45] |
| small_tick | AMD | 120,957 | -0.013 | 0.96 | 0.524 | 107.232 | 1.84 | -0.0099 | 21,030 | 0.41 [0.40, 0.42] |
| small_tick | APP | 44,336 | -0.023 | 0.46 | 0.508 | 612.912 | 2.85 | -0.1268 | 6,001 | 0.43 [0.41, 0.45] |
| small_tick | AVGO | 175,070 | +0.031 | 0.72 | 0.518 | 124.648 | 2.99 | -0.1279 | 27,409 | 0.49 [0.47, 0.50] |
| small_tick | COIN | 63,620 | -0.008 | 0.89 | 0.522 | 235.996 | 2.72 | -0.0598 | 9,294 | 0.44 [0.42, 0.45] |
| small_tick | COST | 40,772 | -0.065 | 0.51 | 0.511 | 232.590 | 2.62 | -0.0420 | 5,595 | 0.38 [0.36, 0.40] |
| small_tick | CRH | 56,713 | +0.065 | -0.02 | 0.529 | 72.618 | 2.21 | -0.0280 | 9,746 | 0.54 [0.51, 0.56] |
| small_tick | CRWV | 109,980 | -0.012 | 0.62 | 0.527 | 91.856 | 1.84 | -0.0090 | 18,812 | 0.45 [0.44, 0.47] |
| small_tick | CVNA | 50,443 | -0.079 | 0.54 | 0.443 | 559.811 | 1.42 | -0.0500 | 8,512 | 0.30 [0.28, 0.32] |
| small_tick | GOOG | 122,089 | -0.074 | 0.59 | 0.516 | 82.278 | 1.77 | -0.0069 | 22,394 | 0.45 [0.44, 0.47] |
| small_tick | HOOD | 80,619 | -0.057 | 0.68 | 0.533 | 95.365 | 2.13 | -0.0123 | 13,692 | 0.48 [0.46, 0.50] |
| small_tick | JPM | 74,186 | -0.044 | 0.35 | 0.502 | 139.185 | 2.20 | -0.0223 | 12,503 | 0.51 [0.48, 0.52] |
| small_tick | LRCX | 47,635 | +0.051 | 0.28 | 0.547 | 91.215 | 2.80 | -0.2070 | 7,723 | 0.45 [0.43, 0.47] |
| small_tick | META | 103,845 | -0.026 | 1.01 | 0.519 | 185.612 | 2.12 | -0.0232 | 16,403 | 0.42 [0.40, 0.43] |
| small_tick | MRVL | 90,761 | +0.029 | 0.56 | 0.525 | 74.442 | 1.88 | -0.0075 | 16,732 | 0.55 [0.53, 0.56] |
| small_tick | MSFT | 167,655 | +0.035 | 0.81 | 0.535 | 81.546 | 2.18 | -0.0112 | 29,300 | 0.58 [0.56, 0.59] |
| small_tick | MSTR | 134,237 | -0.013 | 0.53 | 0.507 | 161.052 | 2.51 | -0.0261 | 20,882 | 0.45 [0.43, 0.46] |
| small_tick | MU | 107,075 | -0.030 | 1.01 | 0.525 | 145.790 | 2.14 | -0.0186 | 17,099 | 0.44 [0.43, 0.45] |
| small_tick | ORCL | 117,494 | -0.092 | 0.46 | 0.474 | 107.089 | 2.21 | -0.0180 | 20,887 | 0.39 [0.37, 0.40] |
| small_tick | PEP | 48,335 | +0.000 | 0.11 | 0.528 | 63.188 | 1.46 | -0.0217 | 8,801 | 0.51 [0.50, 0.53] |
| small_tick | PLTR | 135,143 | +0.017 | 0.63 | 0.519 | 82.995 | 1.86 | -0.0081 | 25,452 | 0.44 [0.43, 0.45] |
| small_tick | QCOM | 47,849 | -0.087 | 0.71 | 0.528 | 79.718 | 2.00 | -0.0123 | 7,765 | 0.47 [0.45, 0.49] |
| small_tick | RKLB | 84,528 | -0.025 | 0.48 | 0.523 | 82.926 | 2.02 | -0.0556 | 16,977 | 0.43 [0.41, 0.44] |
| small_tick | TSLA | 419,479 | +0.010 | 1.21 | 0.528 | 88.875 | 1.82 | -0.0079 | 73,879 | 0.49 [0.48, 0.50] |
| small_tick | TSM | 60,199 | +0.137 | 0.43 | 0.495 | 133.974 | 2.83 | -0.2028 | 10,425 | 0.40 [0.38, 0.42] |

G is in dollars per trade sign; the column divides by nothing, so compare within a stock.

## By group

| Group | Stocks | kappa median | Hurst median | psi median | psi IQR | Kernels failing the check | Attributed runs |
|---|---|---|---|---|---|---|---|
| large_tick | 25 | 0.32 | 0.494 | 0.45 | 0.43 to 0.48 | 25 of 25 | 32 |
| small_tick | 25 | 0.59 | 0.522 | 0.45 | 0.43 to 0.49 | 25 of 25 | 51 |

## Execution schedules under the fitted kernel

Cost of executing X over n equal slots, x' Gamma x / 2 with Gamma_ij = G(|i - j|), relative to TWAP. The empirical kernels do not decay (above), so a decaying power law G(l) = G(1) l^-beta is fitted to lags 1 to 50 of each group's median kernel and used here.

| Group | beta | n | Optimal / TWAP cost | Front-loading x1 / (X/n) |
|---|---|---|---|---|
| large_tick | 0.06 | 10 | 0.998 | -0.86 |
| large_tick | 0.06 | 50 | 0.993 | 6.57 |
| small_tick | 0.07 | 10 | 0.998 | -0.93 |
| small_tick | 0.07 | 50 | 0.991 | 6.14 |

With beta near zero the kernel is close to permanent impact: TWAP is within 1% of the optimum, and the unconstrained optimum alternates in sign (negative front-loading means a first trade against the direction), which a long-only schedule would not allow. The empirical kernels fail the positivity check because the unregularized least-squares G rises at long lags; a regularized or parametric fit is needed before the kernel is used for execution.

## Not built

- Mechanical versus reactive split: needs the simulator with reaction on and off; the queue-reactive simulator failed validation at the current tick (MS7), so the split is not reported.
- Synthetic metaorders of Maitrier, Loeper and Bouchaud, injected metaorders, the latent order book sweeps, Almgren-Chriss and VWAP comparisons in replay, and the half-penny prediction of psi.
- Attributed metaorders: attributed executions are about 0.01% of executions on these stocks, too few to fit.
