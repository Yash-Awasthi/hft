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

| Group | Stocks | kappa median | Hurst median | psi median | psi IQR | Unregularized kernels failing the check | Attributed runs |
|---|---|---|---|---|---|---|---|
| large_tick | 25 | 0.32 | 0.494 | 0.45 | 0.43 to 0.48 | 25 of 25 | 32 |
| small_tick | 25 | 0.59 | 0.522 | 0.45 | 0.43 to 0.49 | 25 of 25 | 51 |

## Parametric kernel

G(l) = G0 (1 + l / l0)^-beta fitted to R(1..250) by bounded least squares (G0 >= 0, beta in [0, 3], l0 in [0.1, 10^4]), the kernel summed over all 500 lags of C. G0 >= 0 and beta >= 0 make G positive, decreasing and convex, so the Toeplitz matrix is positive semi-definite (Polya) by construction; the minimum eigenvalue over lags 0..500 is reported as a check. Fit error: RMSE of R over mean |R|.

| Group | Stock | G0 bp of mid | l0 | beta | Fit error | Min eig | At a bound |
|---|---|---|---|---|---|---|---|
| large_tick | AAL | 39.744 | 2.11 | 0.168 | 0.071 | 1.52e-04 | no |
| large_tick | BAC | 40.973 | 1796.09 | 3.000 | 0.022 | 3.42e-06 | yes |
| large_tick | BKR | 55.036 | 1603.26 | 3.000 | 0.023 | 5.15e-06 | yes |
| large_tick | CARR | 55.164 | 2097.14 | 3.000 | 0.040 | 3.95e-06 | yes |
| large_tick | CFLT | 5.017 | 63.02 | 0.180 | 0.122 | 7.15e-07 | no |
| large_tick | CIFR | 55.107 | 198.14 | 0.733 | 0.027 | 1.02e-05 | no |
| large_tick | CMCSA | 31.410 | 156.01 | 0.000 | 0.062 | -1.73e-15 | yes |
| large_tick | CSCO | 47.662 | 3005.48 | 3.000 | 0.042 | 2.38e-06 | yes |
| large_tick | EXAS | 20.383 | 37.90 | 0.383 | 0.078 | 1.03e-05 | no |
| large_tick | GM | 69.514 | 21.95 | 0.198 | 0.027 | 3.14e-05 | no |
| large_tick | INTC | 38.983 | 63.71 | 0.275 | 0.027 | 8.42e-06 | no |
| large_tick | MDLZ | 46.283 | 297.40 | 0.982 | 0.052 | 7.64e-06 | no |
| large_tick | NFLX | 33.287 | 14.24 | 0.090 | 0.013 | 1.05e-05 | no |
| large_tick | NVDA | 58.474 | 0.65 | 0.085 | 0.011 | 3.13e-04 | no |
| large_tick | ONDS | 66.246 | 0.31 | 0.159 | 0.038 | 1.06e-03 | no |
| large_tick | PYPL | 53.458 | 8065.80 | 3.000 | 0.045 | 9.94e-07 | yes |
| large_tick | RGTI | 56.490 | 0.10 | 0.086 | 0.057 | 8.87e-04 | yes |
| large_tick | RIVN | 48.599 | 0.65 | 0.042 | 0.032 | 1.30e-04 | no |
| large_tick | SMCI | 52.670 | 11.42 | 0.115 | 0.040 | 2.65e-05 | no |
| large_tick | SOFI | 52.915 | 0.43 | 0.222 | 0.058 | 9.50e-04 | no |
| large_tick | VZ | 34.540 | 10000.00 | 0.000 | 0.115 | -1.26e-15 | yes |
| large_tick | WBD | 30.819 | 12.41 | 0.258 | 0.033 | 3.20e-05 | no |
| large_tick | WFC | 66.876 | 2.49 | 0.096 | 0.035 | 1.25e-04 | no |
| large_tick | WMT | 59.530 | 10000.00 | 0.000 | 0.038 | -9.15e-15 | yes |
| large_tick | XOM | 61.918 | 0.10 | 0.040 | 0.048 | 4.74e-04 | yes |
| small_tick | ADBE | 127.239 | 6334.27 | 3.000 | 0.032 | 3.01e-06 | yes |
| small_tick | AMD | 137.486 | 0.10 | 0.093 | 0.012 | 2.32e-03 | yes |
| small_tick | APP | 598.589 | 0.10 | 0.057 | 0.043 | 6.42e-03 | yes |
| small_tick | AVGO | 100.107 | 10000.00 | 0.000 | 0.045 | -4.93e-15 | yes |
| small_tick | COIN | 156.262 | 0.10 | 0.001 | 0.057 | 1.66e-05 | yes |
| small_tick | COST | 209.143 | 0.10 | 0.047 | 0.034 | 1.86e-03 | yes |
| small_tick | CRH | 59.204 | 270.17 | 0.109 | 0.026 | 1.20e-06 | no |
| small_tick | CRWV | 74.160 | 31.17 | 0.101 | 0.022 | 1.20e-05 | no |
| small_tick | CVNA | 397.282 | 5917.38 | 3.000 | 0.046 | 1.01e-05 | yes |
| small_tick | GOOG | 77.103 | 0.10 | 0.031 | 0.027 | 4.59e-04 | yes |
| small_tick | HOOD | 135.527 | 0.10 | 0.080 | 0.041 | 1.99e-03 | yes |
| small_tick | JPM | 134.694 | 301.83 | 0.272 | 0.029 | 6.06e-06 | no |
| small_tick | LRCX | 103.581 | 10000.00 | 0.000 | 0.091 | -7.73e-15 | yes |
| small_tick | META | 210.403 | 0.10 | 0.064 | 0.026 | 2.53e-03 | yes |
| small_tick | MRVL | 78.295 | 5.17 | 0.121 | 0.028 | 9.09e-05 | no |
| small_tick | MSFT | 92.208 | 0.41 | 0.033 | 0.022 | 2.72e-04 | no |
| small_tick | MSTR | 169.307 | 3.75 | 0.129 | 0.021 | 2.86e-04 | no |
| small_tick | MU | 144.609 | 6.60 | 0.144 | 0.016 | 1.58e-04 | no |
| small_tick | ORCL | 108.429 | 0.10 | 0.054 | 0.044 | 1.11e-03 | yes |
| small_tick | PEP | 66.289 | 579.02 | 1.276 | 0.042 | 7.30e-06 | no |
| small_tick | PLTR | 99.678 | 0.10 | 0.080 | 0.027 | 1.47e-03 | yes |
| small_tick | QCOM | 52.734 | 0.10 | 0.008 | 0.055 | 8.71e-05 | yes |
| small_tick | RKLB | 78.154 | 0.10 | 0.018 | 0.020 | 2.68e-04 | yes |
| small_tick | TSLA | 131.534 | 0.10 | 0.119 | 0.018 | 2.78e-03 | yes |
| small_tick | TSM | 113.246 | 477.79 | 0.119 | 0.025 | 1.41e-06 | no |

| Group | Stocks | Parametric kernels failing the check | beta median | Fit error median | At a bound |
|---|---|---|---|---|---|
| large_tick | 25 | 0 of 25 | 0.180 | 0.040 | 10 |
| small_tick | 25 | 0 of 25 | 0.080 | 0.028 | 16 |

## Execution schedules under the parametric kernel

Cost of executing X over n equal slots, x' Gamma x / 2 with Gamma_ij = G(|i - j|), relative to TWAP, with the group's median l0 and beta (G0 cancels in the ratio).

| Group | l0 | beta | n | Optimal / TWAP cost | Front-loading x1 / (X/n) |
|---|---|---|---|---|---|
| large_tick | 37.90 | 0.180 | 10 | 0.996 | 4.50 |
| large_tick | 37.90 | 0.180 | 50 | 0.985 | 16.64 |
| small_tick | 0.41 | 0.080 | 10 | 0.994 | 2.00 |
| small_tick | 0.41 | 0.080 | 50 | 0.992 | 4.32 |

## Not built

- Mechanical versus reactive split: needs the simulator with reaction on and off; the queue-reactive simulator failed validation at the current tick (MS7), so the split is not reported.
- Synthetic metaorders of Maitrier, Loeper and Bouchaud, injected metaorders, the latent order book sweeps, Almgren-Chriss and VWAP comparisons in replay, and the half-penny prediction of psi.
- Attributed metaorders: attributed executions are about 0.01% of executions on these stocks, too few to fit.
