# MS6: quoting and backtest

Validation day 2025-12-11 only; the test days stay locked. 50 universe stocks (25 large-tick, 25 small-tick). Accounting in integer micro-dollars; tables in dollars per stock-day. With a single validation day the 95% intervals resample stocks, not days: they measure dispersion across stocks on one day and say nothing about day-to-day variation.

Attribution: total = spread capture + inventory PnL + fees, checked after every event in the backtest. Inventory PnL is split into adverse selection (mid move over the 1 s after each of our fills, times the fill) and the residual. Fees include rebates, access fees, Section 31 and FINRA TAF (`configs/fees.toml`).

Sharpe: mean over standard deviation of the group's summed PnL per 5-minute bucket (about 78 buckets per day; not annualized). Deflated Sharpe (Bailey and Lopez de Prado): probability that the true Sharpe exceeds the expected maximum of 31 unskilled trials (distinct Q1 configurations in the registry), Sharpe variance across trials 0.0767.

## Strategies (latency 0, 30 mil cap, queue fill rule)

| Group | Strategy | Stock-days | PnL $ [95% CI] | Spread $ | Adverse $ | Residual inv. $ | Fees $ | Fills | Volume | Max inv. mean / max | PnL c/share | Sharpe 5 min | DSR |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| large_tick | as | 25 | -54.28 [-94.79, -14.35] | 109.87 | -168.10 | -0.50 | 4.45 | 217 | 8,846 | 336 / 572 | -0.614 | -0.213 | 0.000 |
| large_tick | dp | 25 | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 0 | 0 / 0 | 0.000 | - | - |
| large_tick | dp_signal | 25 | 0.61 [-3.47, 5.98] | 0.77 | -0.88 | 0.77 | -0.06 | 2 | 67 | 16 / 192 | 0.906 | 0.010 | 0.000 |
| large_tick | ext | 25 | -0.99 [-7.83, 5.98] | 0.93 | -0.43 | -1.44 | -0.05 | 1 | 59 | 16 / 188 | -1.673 | -0.009 | 0.000 |
| large_tick | naive | 25 | -552.64 [-1,128.46, -163.86] | 2,242.19 | -3,266.94 | 67.11 | 405.00 | 10,324 | 439,948 | 611 / 683 | -0.126 | -0.558 | 0.000 |
| small_tick | as | 25 | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 0 | 0 / 0 | 0.000 | - | - |
| small_tick | dp | 25 | -4,007.75 [-4,611.33, -3,390.84] | 8,311.24 | -11,233.02 | -650.61 | -435.35 | 8,172 | 244,726 | 277 / 336 | -1.638 | -0.663 | 0.000 |
| small_tick | dp_signal | 25 | -3,858.30 [-4,479.68, -3,177.37] | 7,870.93 | -10,627.09 | -636.12 | -466.02 | 8,371 | 243,618 | 350 / 482 | -1.584 | -0.626 | 0.000 |
| small_tick | ext | 25 | -4,175.87 [-4,806.56, -3,619.71] | 6,079.50 | -8,858.39 | -649.80 | -747.19 | 8,834 | 282,758 | 670 / 994 | -1.477 | -0.595 | 0.000 |
| small_tick | naive | 25 | -2,581.64 [-3,336.93, -1,859.64] | 5,918.56 | -7,590.30 | -448.50 | -461.40 | 8,718 | 258,383 | 635 / 697 | -0.999 | -0.345 | 0.000 |

Strategies that sent no order on any stock-day (mean risk rejects per stock-day): large_tick dp (0); small_tick as (2,788,858).

AS base quote distance from the mid (gamma 0.05): large_tick k = 0.501/tick, 1.9 ticks; small_tick k = 0.027/tick, 21.0 ticks. Orders more than 20 ticks from the mid are rejected by the risk collar.

## Sanity strategies

| Group | Strategy | PnL $ [95% CI] | Spread $ | Adverse $ | Fees $ | Fills | PnL c/share |
|---|---|---|---|---|---|---|---|
| large_tick | foresight | 5,808.21 [3,141.02, 9,877.32] | -9,477.12 | 22,710.58 | -6,788.90 | 32,473 | 0.363 |
| large_tick | random_passive | -200.02 [-502.70, 16.89] | 1,115.45 | -1,583.56 | 129.60 | 5,334 | -0.092 |
| large_tick | random_taker | -725.83 [-1,126.16, -440.47] | -464.14 | -28.41 | -296.31 | 1,629 | -1.056 |
| large_tick | zero | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| small_tick | foresight | 26,495.57 [18,453.35, 35,537.61] | -33,437.94 | 69,603.88 | -7,833.44 | 27,487 | 2.353 |
| small_tick | random_passive | -1,662.59 [-2,505.60, -808.45] | 3,250.03 | -4,024.80 | -253.56 | 4,643 | -1.187 |
| small_tick | random_taker | -2,570.15 [-3,484.03, -1,674.23] | -1,763.34 | -67.73 | -310.08 | 1,426 | -5.253 |
| small_tick | zero | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |

## Latency (ext)

Market-data and order-entry latency both set to the value; processing 5 us.

| Group | Latency us | PnL $ | Fills | PnL c/share |
|---|---|---|---|---|
| large_tick | 0 | -0.99 | 1 | -1.673 |
| large_tick | 10 | -2.03 | 1 | -3.763 |
| large_tick | 50 | -13.17 | 2 | -16.135 |
| large_tick | 100 | -11.88 | 2 | -15.056 |
| large_tick | 250 | -13.50 | 3 | -12.441 |
| large_tick | 500 | -16.46 | 3 | -12.576 |
| small_tick | 0 | -4,175.87 | 8,834 | -1.477 |
| small_tick | 10 | -4,420.01 | 8,355 | -1.682 |
| small_tick | 50 | -4,227.36 | 7,885 | -1.740 |
| small_tick | 100 | -4,233.07 | 7,734 | -1.780 |
| small_tick | 250 | -3,790.62 | 7,465 | -1.672 |
| small_tick | 500 | -3,947.37 | 7,358 | -1.776 |

## Fee schedule and fill rule

| Group | Strategy | Fees | Fill rule | PnL $ [95% CI] | Spread $ | Adverse $ | Fees $ | Fills | PnL c/share |
|---|---|---|---|---|---|---|---|---|---|
| large_tick | as | cap10 | queue | -65.50 [-110.07, -23.29] | 109.87 | -168.10 | -6.77 | 217 | -0.740 |
| large_tick | as | cap10 | trade_through | 14.82 [-20.89, 59.92] | 31.22 | -77.64 | -2.82 | 78 | 0.511 |
| large_tick | as | cap30 | queue | -54.28 [-94.15, -15.22] | 109.87 | -168.10 | 4.45 | 217 | -0.614 |
| large_tick | as | cap30 | trade_through | 18.22 [-17.78, 62.79] | 31.22 | -77.64 | 0.59 | 78 | 0.628 |
| large_tick | dp | cap10 | queue | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| large_tick | dp | cap10 | trade_through | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| large_tick | dp | cap30 | queue | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| large_tick | dp | cap30 | trade_through | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| large_tick | dp_signal | cap10 | queue | 0.54 [-3.77, 5.97] | 0.77 | -0.88 | -0.12 | 2 | 0.814 |
| large_tick | dp_signal | cap10 | trade_through | -0.41 [-1.24, 0.00] | -0.01 | -0.05 | -0.02 | 0 | -5.169 |
| large_tick | dp_signal | cap30 | queue | 0.61 [-3.61, 5.98] | 0.77 | -0.88 | -0.06 | 2 | 0.906 |
| large_tick | dp_signal | cap30 | trade_through | -0.41 [-1.22, 0.00] | -0.01 | -0.05 | -0.01 | 0 | -5.097 |
| large_tick | ext | cap10 | queue | -1.04 [-8.05, 5.41] | 0.93 | -0.43 | -0.10 | 1 | -1.765 |
| large_tick | ext | cap10 | trade_through | -0.41 [-1.24, 0.00] | -0.01 | -0.05 | -0.02 | 0 | -5.169 |
| large_tick | ext | cap30 | queue | -0.99 [-7.87, 5.43] | 0.93 | -0.43 | -0.05 | 1 | -1.673 |
| large_tick | ext | cap30 | trade_through | -0.41 [-1.22, 0.00] | -0.01 | -0.05 | -0.01 | 0 | -5.097 |
| large_tick | naive | cap10 | queue | -928.60 [-1,282.60, -612.84] | 2,146.99 | -3,131.03 | -126.72 | 9,677 | -0.222 |
| large_tick | naive | cap10 | trade_through | -147.55 [-427.99, 65.09] | -47.06 | -133.13 | -22.64 | 641 | -0.668 |
| large_tick | naive | cap30 | queue | -552.64 [-1,138.87, -166.79] | 2,242.19 | -3,266.94 | 405.00 | 10,324 | -0.126 |
| large_tick | naive | cap30 | trade_through | -119.66 [-369.07, 77.63] | -47.06 | -133.13 | 5.24 | 641 | -0.542 |
| small_tick | as | cap10 | queue | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| small_tick | as | cap10 | trade_through | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| small_tick | as | cap30 | queue | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| small_tick | as | cap30 | trade_through | 0.00 [0.00, 0.00] | 0.00 | 0.00 | 0.00 | 0 | 0.000 |
| small_tick | dp | cap10 | queue | -3,978.83 [-4,533.72, -3,380.43] | 7,887.27 | -10,632.68 | -701.94 | 7,662 | -1.736 |
| small_tick | dp | cap10 | trade_through | -4,279.93 [-4,864.18, -3,705.16] | 5,907.68 | -9,038.38 | -544.79 | 5,817 | -2.474 |
| small_tick | dp | cap30 | queue | -4,007.75 [-4,604.67, -3,417.16] | 8,311.24 | -11,233.02 | -435.35 | 8,172 | -1.638 |
| small_tick | dp | cap30 | trade_through | -4,210.09 [-4,826.29, -3,586.90] | 6,094.24 | -9,335.14 | -338.30 | 6,082 | -2.327 |
| small_tick | dp_signal | cap10 | queue | -3,740.31 [-4,314.10, -3,153.44] | 7,548.69 | -10,161.93 | -721.63 | 7,864 | -1.631 |
| small_tick | dp_signal | cap10 | trade_through | -4,175.59 [-4,796.74, -3,555.28] | 5,472.85 | -8,411.06 | -542.43 | 5,927 | -2.451 |
| small_tick | dp_signal | cap30 | queue | -3,858.30 [-4,468.36, -3,184.38] | 7,870.93 | -10,627.09 | -466.02 | 8,371 | -1.584 |
| small_tick | dp_signal | cap30 | trade_through | -3,714.46 [-4,249.97, -3,183.31] | 5,663.70 | -8,738.12 | -346.61 | 6,217 | -2.077 |
| small_tick | ext | cap10 | queue | -3,840.33 [-4,427.37, -3,206.68] | 5,425.16 | -8,145.24 | -893.04 | 8,627 | -1.355 |
| small_tick | ext | cap10 | trade_through | -4,427.09 [-5,008.89, -3,823.00] | 3,941.78 | -6,977.88 | -741.45 | 6,967 | -1.917 |
| small_tick | ext | cap30 | queue | -4,175.87 [-4,783.38, -3,632.78] | 6,079.50 | -8,858.39 | -747.19 | 8,834 | -1.477 |
| small_tick | ext | cap30 | trade_through | -4,396.33 [-4,944.24, -3,839.37] | 4,271.12 | -7,313.15 | -623.30 | 6,795 | -2.003 |
| small_tick | naive | cap10 | queue | -2,816.57 [-3,576.26, -2,096.43] | 5,667.71 | -7,267.05 | -706.68 | 7,907 | -1.176 |
| small_tick | naive | cap10 | trade_through | -251.50 [-1,176.14, 641.30] | 664.06 | -1,516.50 | -163.56 | 1,601 | -0.539 |
| small_tick | naive | cap30 | queue | -2,581.64 [-3,313.03, -1,861.77] | 5,918.56 | -7,590.30 | -461.40 | 8,718 | -0.999 |
| small_tick | naive | cap30 | trade_through | -191.92 [-1,109.61, 696.53] | 664.06 | -1,516.50 | -103.98 | 1,601 | -0.411 |

## Ablation

| Group | Strategy | Removed | PnL $ [95% CI] | Change vs ext $ | Fills | PnL c/share |
|---|---|---|---|---|---|---|
| large_tick | ext | - | -0.99 [-7.87, 5.43] | 0.00 | 1 | -1.673 |
| large_tick | ext_no_toxicity | toxicity guard | 0.61 [-3.61, 5.98] | 1.59 | 2 | 0.906 |
| large_tick | ext_no_taking | aggressive taking | -0.99 [-7.87, 5.43] | -0.00 | 1 | -1.673 |
| large_tick | dp_signal | both extensions | 0.61 [-3.61, 5.98] | 1.59 | 2 | 0.906 |
| large_tick | dp | extensions and signal | 0.00 [0.00, 0.00] | 0.99 | 0 | 0.000 |
| small_tick | ext | - | -4,175.87 [-4,783.38, -3,632.78] | 0.00 | 8,834 | -1.477 |
| small_tick | ext_no_toxicity | toxicity guard | -3,942.32 [-4,378.13, -3,471.06] | 233.55 | 9,181 | -1.357 |
| small_tick | ext_no_taking | aggressive taking | -3,888.62 [-4,516.63, -3,241.39] | 287.25 | 8,117 | -1.633 |
| small_tick | dp_signal | both extensions | -3,858.30 [-4,468.36, -3,184.38] | 317.57 | 8,371 | -1.584 |
| small_tick | dp | extensions and signal | -4,007.75 [-4,604.67, -3,417.16] | 168.13 | 8,172 | -1.638 |

Determinism: 150 of 150 jobs repeated across the main and ablation sweeps give identical totals.

## Not built

Deep queue reservation (resting orders behind the best to hold queue position), portfolio-level inventory across stocks, and online recalibration of the fill intensity A and decay k (sigma is an online EWMA; A and k are fixed from the train days).

## Avellaneda-Stoikov closed forms against the numerical HJB

Parameters of the baseline: A = 1.0/s, k = 1.5/tick, gamma = 0.05, sigma^2 = 0.05 ticks^2/s, inventory -5..5 lots. Bid distance from the mid in ticks; errors are the maximum over inventories that quote a bid. The numerical solution maximizes the Hamiltonian by golden section and integrates the HJB backward with RK4.

| Horizon s | Numerical vs exact (max, ticks) | Exact bid q=0 | GLFT asymptotic q=0 | AS q=0 | GLFT max error | AS max error |
|---|---|---|---|---|---|---|
| 1 | 1.0e-15 | 0.657 | 0.680 | 0.657 | 0.134 | 0.172 |
| 60 | 7.1e-15 | 0.687 | 0.680 | 0.731 | 0.259 | 0.254 |
| 600 | 5.7e-14 | 0.687 | 0.680 | 1.406 | 0.259 | 6.275 |
| 3,600 | 2.3e-13 | 0.687 | 0.680 | 5.156 | 0.259 | 40.025 |
| 23,400 | 1.8e-12 | 0.687 | 0.680 | 29.906 | 0.259 | 262.775 |
