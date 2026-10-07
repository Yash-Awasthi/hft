# MS7: regime forecast (half-penny tick, 10 mil cap)

Inputs: per-stock day statistics of the train days (fit) and the validation day (scoring at the current tick); 3,801 Nasdaq common stocks, 871 treated by the Nasdaq-only proxy (mid at least $1, time-weighted spread at most $0.015). Forecasts for the 19 treated universe stocks; the universe's 31 untreated stocks keep the current tick. Bundle: `docs/prereg/` (forecast, manifest with hashes, scoring rules).

## Method A fit

| Target | Leave-stocks-out R2 | MAE (log) | Validation-day R2 |
|---|---|---|---|
| spread in ticks | 0.939 | 0.270 | 0.926 |
| depth at best / shares per minute | 0.878 | 0.348 | 0.863 |
| displayed executions per day / depth | 0.869 | 0.363 | 0.862 |

## Against past tick changes

The causal step cannot be tested on one tick regime; the methods are checked against the direction and size of published effects. Inputs are our stocks, not the original populations.

| Change | Measure | Published | Method A | Method B |
|---|---|---|---|---|
| Tick Size Pilot, x5 (TG1 vs controls) | quoted spread | +17% | +31% | +0% |
| Tick Size Pilot, x5 | depth | +275% | +293% | - |
| Tokyo 2014 phase I (TOPIX 100, smaller tick) | effective spread | -36% | -9% (halving, treated) | -49% |
| Tokyo 2014 phase II | effective spread | -56% | | |
| Tokyo 2014 | depth | decreased | -45% | - |

Pilot-like stocks: 1140 ($5 to $50, notional below the median). The Tokyo tick reductions differed by price band and were larger than a halving, so only the direction is comparable.

## Combination

Weights are inverse squared median absolute log errors predicting the validation day at the current tick, on the same stocks. At the current tick these stocks quote about one tick, so B and C predict the spread level almost trivially; the weights measure level accuracy, not the accuracy of the tick response, which only the past tick changes speak to.

| Target | Method | Held-out error | Weight |
|---|---|---|---|
| spread | a | 0.431 | 0.01 |
| spread | b | 0.051 | 0.46 |
| spread | c | 0.047 | 0.53 |
| depth | a | 0.646 | 0.88 |
| depth | c | 1.754 | 0.12 |

## Forecast per treated universe stock

Spread in cents, depth in shares at the best (mean of the two sides). Bands span every method's estimate and its own band (A: 90% stock bootstrap; B: beta 1/2 to 1; C: even or inner-weighted redistribution). D marks methods that disagree (opposite signs or more than a factor 1.5 apart).

| Stock | Spread now | Half: combined [band] | A | B | C | Depth now | Half: combined [band] | Turnover x (A) | Latent share |
|---|---|---|---|---|---|---|---|---|---|
| AAL | 1.006 | 0.713 [0.517, 1.067] D | 1.041 | 1.006 | 0.528 | 7,179 | 3,744 [1,115, 4,577] D | 1.61 | 17.2% |
| BAC | 1.051 | 0.633 [0.599, 1.275] D | 1.238 | 0.599 | 0.658 | 883 | 543 [87, 718] D | 1.17 | 19.2% |
| CFLT | 1.004 | 0.512 [0.500, 0.897] D | 0.872 | 0.500 | 0.518 | 25,535 | 13,220 [9,624, 14,525] | 1.92 | 20.2% |
| CIFR | 1.360 | 1.225 [0.698, 1.986] D | 1.444 | 0.698 | 1.986 | 561 | 405 [106, 502] D | 1.05 | 8.7% |
| CMCSA | 1.004 | 0.566 [0.507, 1.121] D | 1.094 | 0.630 | 0.512 | 3,403 | 1,898 [562, 2,304] D | 1.48 | 20.4% |
| CSCO | 1.110 | 0.756 [0.634, 1.322] D | 1.286 | 0.634 | 0.875 | 453 | 301 [60, 386] D | 1.11 | 16.5% |
| EXAS | 1.239 | 0.800 [0.500, 1.277] D | 1.245 | 0.500 | 1.194 | 1,013 | 545 [264, 627] D | 1.68 | 6.6% |
| INTC | 1.078 | 0.723 [0.611, 1.222] D | 1.192 | 0.611 | 0.831 | 1,752 | 1,196 [229, 1,541] D | 1.07 | 12.5% |
| MDLZ | 1.160 | 1.095 [0.926, 1.404] D | 1.364 | 1.160 | 1.040 | 429 | 274 [54, 353] D | 1.16 | 12.6% |
| NFLX | 1.156 | 0.681 [0.587, 1.122] D | 1.091 | 0.587 | 0.770 | 254 | 202 [93, 235] D | 1.04 | 11.0% |
| NVDA | 1.097 | 0.934 [0.725, 1.097] | 1.018 | 1.097 | 0.812 | 368 | 299 [137, 345] D | 1.06 | 6.8% |
| ONDS | 1.034 | 0.808 [0.610, 1.254] D | 1.218 | 1.034 | 0.649 | 2,626 | 1,638 [298, 2,127] D | 1.18 | 12.3% |
| PYPL | 1.405 | 1.672 [1.405, 1.944] | 1.592 | 1.405 | 1.944 | 333 | 234 [61, 289] D | 1.08 | 7.9% |
| RGTI | 1.253 | 1.044 [0.645, 1.577] D | 1.262 | 0.645 | 1.577 | 461 | 321 [59, 418] D | 1.04 | 7.9% |
| RIVN | 1.045 | 0.822 [0.613, 1.267] D | 1.229 | 1.045 | 0.665 | 1,372 | 832 [137, 1,097] D | 1.20 | 17.4% |
| SMCI | 1.220 | 1.339 [1.220, 1.450] | 1.379 | 1.220 | 1.450 | 413 | 285 [63, 360] D | 1.08 | 9.0% |
| SOFI | 1.034 | 0.573 [0.502, 1.253] D | 1.217 | 0.502 | 0.636 | 2,013 | 1,360 [417, 1,647] D | 1.17 | 7.5% |
| VZ | 1.032 | 0.634 [0.570, 1.216] D | 1.182 | 0.662 | 0.607 | 1,757 | 1,028 [234, 1,292] D | 1.33 | 26.9% |
| WBD | 1.028 | 0.562 [0.514, 1.247] D | 1.211 | 0.514 | 0.601 | 3,119 | 2,026 [479, 2,541] D | 1.17 | 15.0% |

## Method C: fills, markouts and market-maker PnL (simulated)

Probability that an order joining the best with less than one AES ahead fills within 10 s, mean 1 s markout of fills (cents, positive is good for the resting order), and the naive strategy's PnL per share in the backtest (30 mil fees now, 10 mil cap at the half tick).

| Stock | Fill 10 s now | half | Markout now | half | Naive c/share now | half |
|---|---|---|---|---|---|---|
| AAL | - | 0.215 | 0.484 | -0.083 | 0.671 | -0.035 |
| BAC | - | 0.220 | 0.496 | 0.020 | 0.676 | 0.032 |
| CFLT | 0.404 | 0.478 | 0.461 | 0.127 | 0.514 | -0.183 |
| CIFR | 0.148 | 0.106 | 0.348 | 0.242 | 0.498 | 0.411 |
| CMCSA | - | 0.289 | 0.497 | -0.071 | 0.694 | -0.163 |
| CSCO | 0.235 | 0.179 | 0.485 | 0.044 | 0.649 | 0.068 |
| EXAS | 0.256 | 0.257 | 0.278 | 0.094 | 0.359 | 0.248 |
| INTC | 0.158 | 0.165 | 0.495 | 0.063 | 0.674 | 0.066 |
| MDLZ | 0.233 | 0.212 | 0.468 | 0.032 | 0.546 | 0.209 |
| NFLX | 0.457 | 0.391 | 0.165 | 0.091 | 0.302 | 0.122 |
| NVDA | 0.231 | 0.131 | 0.119 | -0.003 | 0.237 | -0.097 |
| ONDS | 0.097 | 0.156 | 0.476 | -0.010 | 0.627 | 0.075 |
| PYPL | 0.266 | 0.179 | 0.394 | 0.125 | 0.515 | 0.365 |
| RGTI | 0.158 | 0.124 | 0.366 | 0.223 | 0.469 | 0.282 |
| RIVN | 0.192 | 0.157 | 0.480 | 0.021 | 0.665 | 0.125 |
| SMCI | 0.212 | 0.144 | 0.374 | 0.174 | 0.446 | 0.309 |
| SOFI | 0.230 | 0.156 | 0.427 | 0.021 | 0.552 | -0.082 |
| VZ | - | 0.204 | 0.499 | 0.001 | 0.696 | -0.068 |
| WBD | 0.340 | 0.305 | 0.465 | -0.023 | 0.599 | -0.003 |

## Limitations

- Method C fails validation at the current tick: median absolute log error of depth 1.75 (a factor 5.8); simulated reference-price moves per session range 10 to 28,189 (INTC on the validation day: 7,241 real moves). Calibrated level-1 flux is net positive at every queue size (INTC: about 9.8 AES/s in against 7.3 out), yet real level-1 queues empty about 12,300 times a day; model I has no dependence on the opposite queue (HLR models II and III), which is the next step. Its fill, markout and PnL columns are therefore not forecasts; they are reported only as the simulator's output.
- Not built: the power-law feedback kernel of Noble et al., the DP policy re-solved at the new tick, and the queue value term for lower rebates. Fill probability by queue position, markouts and market-maker PnL per share have no validated forecast.
- Method B covers only the spread; method A's bands are a stock bootstrap within one regime.
- The Tokyo comparison uses aggregate effective spreads (CMCRC) whose tick factors differ by price band.

## Latent demand inside the tick

Median share of executed shares that are hidden fills at sub-penny prices: treated 19.8%, untreated 5.4%.
