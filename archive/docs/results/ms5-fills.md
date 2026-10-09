# MS5: fills and adverse selection

## Queue tracking: real orders re-inserted at their own position

`apps/reinsert` takes one real order in 50 (by reference hash) out of the replayed book from
its arrival and keeps it as a virtual order at the same queue position; its own executions,
cancels and deletes in the data then apply to the copy. 2025-12-08, the 50 universe symbols.

| Fill rule | Mirrored | Executed in data | Reproduced exactly | Filled early | Executions while tracked queue-ahead > 0 |
|---|---|---|---|---|---|
| queue (default) | 420,923 | 40,158 | 38,851 (96.7%) | 3,932 | 1,226 of 47,323 (2.6%) |
| trade-through (conservative) | 420,923 | 40,158 | 39,995 (99.6%) | 102 | 1,257 of 47,323 (2.7%) |

Findings:
- Queue priority follows the order reference, not the order of add messages: orders
  received before the open are displayed at 09:30 but keep their earlier rank. Ranking by
  arrival message instead raised the queue-ahead violations on NVDA from 209 to 281.
- The tracked queue-ahead equals a walk of the book in every inspected case. The remaining
  executions with shares still ahead are round lots executed ahead of odd lots queued
  earlier, consistent with incoming orders carrying a minimum-quantity condition, which ITCH
  does not show. They cannot be modelled from this feed.
- Under the queue rule, fills the data does not give come from executions of orders queued
  behind (201,532 shares) and hidden prints at our price (51,016 shares), where the queue
  rule assumes displayed priority. The conservative rule removes almost all of them; the two
  rules bound the fill assumption (DESIGN.md section 2).
- An L2-only estimate of shares ahead (executions from the front, cancels pro rata) is off
  by 14,169 shares on average against an exact mean of 73,750 (19%), sampled at every
  message that changes the order's level: the value of order-level data for queue position.

## Fill model, markouts and queue value

Train orders: 1509322 (['<store-dir>/store-S120825-v50', '<store-dir>/store-S120925-v50', '<store-dir>/store-S121025-v50']); validation orders: 547205 (<store-dir>/store-S121125-v50). One order in 20 sampled by reference; horizon 60 s.

Validation outcomes (0 censored, 1 fill, 2 away): 0: 276574, 1: 52721, 2: 217910

### Cause-specific Cox models (train days)

| Covariate | Fill log-HR | Away log-HR |
|---|---|---|
| log_ahead | -0.222 ± 0.008 | +0.008 ± 0.005 |
| log_opposite | +0.058 ± 0.016 | -0.033 ± 0.009 |
| imbalance_own | +0.116 ± 0.045 | +0.253 ± 0.025 |
| spread | -0.110 ± 0.004 | +0.016 ± 0.001 |
| volatility | +0.331 ± 0.022 | -0.256 ± 0.019 |
| signal_own | -5.005 ± 0.618 | +7.027 ± 0.252 |
| log_shares | -0.034 ± 0.012 | -0.016 ± 0.006 |
| small_tick | +0.080 ± 0.030 | +0.230 ± 0.016 |

### Calibration on validation-day real orders

Predicted fill probability by tau (cumulative incidence) against the Aalen-Johansen estimate, by decile of the prediction.

| tau | Decile | Predicted | Observed | Orders |
|---|---|---|---|---|
| 0.01 s | 1 | 0.015 | 0.019 | 54721 |
| 0.01 s | 2 | 0.026 | 0.018 | 54720 |
| 0.01 s | 3 | 0.032 | 0.017 | 54721 |
| 0.01 s | 4 | 0.037 | 0.021 | 54720 |
| 0.01 s | 5 | 0.041 | 0.027 | 54720 |
| 0.01 s | 6 | 0.046 | 0.033 | 54721 |
| 0.01 s | 7 | 0.051 | 0.040 | 54720 |
| 0.01 s | 8 | 0.058 | 0.054 | 54721 |
| 0.01 s | 9 | 0.078 | 0.071 | 54720 |
| 0.01 s | 10 | 0.132 | 0.195 | 54721 |
| 0.01 s | mean abs error | 0.015 | | |
| 0.1 s | 1 | 0.021 | 0.025 | 54721 |
| 0.1 s | 2 | 0.038 | 0.026 | 54720 |
| 0.1 s | 3 | 0.046 | 0.027 | 54721 |
| 0.1 s | 4 | 0.053 | 0.033 | 54720 |
| 0.1 s | 5 | 0.060 | 0.045 | 54720 |
| 0.1 s | 6 | 0.066 | 0.055 | 54721 |
| 0.1 s | 7 | 0.074 | 0.070 | 54720 |
| 0.1 s | 8 | 0.085 | 0.087 | 54721 |
| 0.1 s | 9 | 0.113 | 0.106 | 54720 |
| 0.1 s | 10 | 0.188 | 0.241 | 54721 |
| 0.1 s | mean abs error | 0.015 | | |
| 1 s | 1 | 0.038 | 0.047 | 54721 |
| 1 s | 2 | 0.068 | 0.052 | 54720 |
| 1 s | 3 | 0.084 | 0.059 | 54721 |
| 1 s | 4 | 0.097 | 0.075 | 54720 |
| 1 s | 5 | 0.109 | 0.099 | 54720 |
| 1 s | 6 | 0.122 | 0.122 | 54721 |
| 1 s | 7 | 0.137 | 0.147 | 54720 |
| 1 s | 8 | 0.157 | 0.175 | 54721 |
| 1 s | 9 | 0.202 | 0.194 | 54720 |
| 1 s | 10 | 0.325 | 0.339 | 54721 |
| 1 s | mean abs error | 0.013 | | |
| 10 s | 1 | 0.065 | 0.101 | 54721 |
| 10 s | 2 | 0.121 | 0.113 | 54720 |
| 10 s | 3 | 0.150 | 0.131 | 54721 |
| 10 s | 4 | 0.174 | 0.153 | 54720 |
| 10 s | 5 | 0.198 | 0.185 | 54720 |
| 10 s | 6 | 0.223 | 0.220 | 54721 |
| 10 s | 7 | 0.251 | 0.256 | 54720 |
| 10 s | 8 | 0.286 | 0.276 | 54721 |
| 10 s | 9 | 0.345 | 0.317 | 54720 |
| 10 s | 10 | 0.493 | 0.433 | 54721 |
| 10 s | mean abs error | 0.020 | | |

### Markouts of fills (validation day), ticks

theta (m(t + tau) - p); negative means adverse selection. 95% intervals.

| Group | Mechanism | Fills | 0.001 s | 0.01 s | 0.1 s | 1 s | 10 s | 60 s |
|---|---|---|---|---|---|---|---|---|
| large_tick | all | 32499 | -0.189 ± 0.008 | -0.217 ± 0.011 | -0.226 ± 0.015 | -0.258 ± 0.026 | -0.249 ± 0.067 | -0.204 ± 0.133 |
| large_tick | sweep | 10042 | -0.372 ± 0.013 | -0.409 ± 0.018 | -0.417 ± 0.027 | -0.425 ± 0.051 | -0.375 ± 0.126 | -0.453 ± 0.244 |
| large_tick | front, not sweep | 22457 | -0.108 ± 0.010 | -0.131 ± 0.013 | -0.141 ± 0.018 | -0.184 ± 0.030 | -0.192 ± 0.080 | -0.092 ± 0.158 |
| large_tick | cancel within 1 ms | 1167 | +0.153 ± 0.041 | +0.177 ± 0.051 | +0.180 ± 0.077 | -0.040 ± 0.148 | +0.134 ± 0.403 | +0.746 ± 0.825 |
| small_tick | all | 30261 | -0.165 ± 0.056 | -0.285 ± 0.073 | -0.378 ± 0.083 | -0.354 ± 0.111 | -0.986 ± 0.282 | -1.801 ± 0.618 |
| small_tick | sweep | 13992 | -0.758 ± 0.082 | -0.799 ± 0.111 | -0.825 ± 0.122 | -0.800 ± 0.175 | -1.126 ± 0.430 | -1.760 ± 0.935 |
| small_tick | front, not sweep | 16269 | +0.344 ± 0.075 | +0.157 ± 0.096 | +0.006 ± 0.113 | +0.029 ± 0.142 | -0.866 ± 0.372 | -1.836 ± 0.821 |
| small_tick | cancel within 1 ms | 884 | +1.424 ± 0.307 | +1.314 ± 0.377 | +1.170 ± 0.418 | +1.134 ± 0.725 | +1.920 ± 1.610 | +0.371 ± 3.826 |

### Queue value by queue position at arrival (validation day)

W = P(fill within 60 s) x (markout at 1 s + rebate of 0.20 ticks).

| Group | Shares ahead | Orders | P(fill) | Markout 1 s | W (ticks) |
|---|---|---|---|---|---|
| large_tick | 0-66 | 61756 | 0.393 | -0.282 | -0.0321 |
| large_tick | 66-176 | 62087 | 0.313 | -0.270 | -0.0220 |
| large_tick | 176-391 | 61852 | 0.282 | -0.299 | -0.0278 |
| large_tick | 391-1064 | 61746 | 0.230 | -0.209 | -0.0021 |
| large_tick | 1064-182807 | 61759 | 0.201 | -0.161 | +0.0079 |
| small_tick | 0-14 | 95639 | 0.258 | -0.375 | -0.0452 |
| small_tick | 14-46 | 48818 | 0.165 | -0.238 | -0.0062 |
| small_tick | 46-105 | 49117 | 0.163 | -0.137 | +0.0103 |
| small_tick | 105-30325 | 47725 | 0.165 | -0.578 | -0.0622 |

Caveats: one validation day; markout intervals treat fills as independent within a day.
Real orders' cancels are informative censoring. ITCH covers Nasdaq only, so fills traded
through on other venues are invisible.
