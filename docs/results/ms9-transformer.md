# MS9: event transformer and in-loop inference kernel

Model: d_model 32, 2 layers, 2 heads, MLP 64, sliding window 64 with an ALiBi bias, 21,354 parameters (83.4 KB in float32; the design's 10k parameters / 40 KB in L1 is missed by about 2x). Trained on the train days for 10 large-tick universe stocks (NVDA, NFLX, INTC, WBD, WMT, SOFI, XOM, CFLT, CSCO, WFC), 3,000 steps of 32 x 1,024 events.

## Validation day (first 400 chunks of 1,024 events)

| Step | Forecast CE (nats, 2 horizons) | IC, 10 events | Direction accuracy, non-flat, 10 events | Generator CE | Unigram CE |
|---|---|---|---|---|---|
| 500 | 0.906 | 0.358 | 72.3% | 2.693 | 3.112 |
| 1000 | 0.903 | 0.381 | 73.9% | 2.635 | 3.112 |
| 1500 | 0.890 | 0.398 | 74.6% | 2.599 | 3.112 |
| 2000 | 0.891 | 0.403 | 75.3% | 2.585 | 3.112 |
| 2500 | 0.886 | 0.406 | 75.2% | 2.570 | 3.112 |
| 3000 | 0.884 | 0.409 | 75.5% | 2.568 | 3.112 |

The generator's next-event cross entropy is 0.544 nats below the unigram baseline. The forecast horizon is in events, so the IC is not comparable with the clock-time ICs of MS4.

## Kernel against PyTorch

200,000 consecutive validation-day events of INTC, trained weights.

| Path | Max abs logit error | Same arg-max, 10 events | Same arg-max, 100 events | Same next-event class |
|---|---|---|---|---|
| scalar | 6.7e-06 | 100.0000% | 100.0000% | 100.0000% |
| avx2 | 6.7e-06 | 100.0000% | 100.0000% | 99.9995% |

Golden test in CI (`EventTransformer.MatchesPyTorchAndAgreesOnDecisions`): random weights, 300 events, window 32 so the ring wraps; error below 2e-5 and identical forecast decisions on both paths.

## Step latency (provisional, see STATUS item 1)

| Path | Median ns per event | Target |
|---|---|---|
| Scalar C++ | 9,713 | - |
| AVX2 float32 | 2,494 | < 2,000 (miss) |

`hft_bench --benchmark_filter=EventStep` with `HFT_TRANSFORMER` pointing at the trained weights; window full. Measured while a DP solve ran on other cores.

## Small model

`python research/transformer.py ... --size small`: d_model 24, 2 layers, 3 heads (head size 8), MLP 24, window 64; 10,618 parameters (41.5 KB in float32), same data and 3,000 steps.

| Model | Params | Forecast CE | IC, 10 events | Direction accuracy, 10 events | Generator CE |
|---|---|---|---|---|---|
| Base | 21,354 | 0.884 | 0.409 | 75.5% | 2.568 |
| Small | 10,618 | 0.888 | 0.403 | 74.7% | 2.584 |

Kernel against PyTorch, 200,000 INTC validation events: max abs logit error 1.6e-5 (scalar) and 1.8e-5 (AVX2); same arg-max at 10 events 100% / 99.9995%, at 100 events 100% on both paths, same next-event class 100%.

Step latency, both models measured back to back while a 4-worker backtest sweep ran (provisional):

| Model | Scalar median ns | AVX2 median ns | Target |
|---|---|---|---|
| Base | - | 2,125 | < 2,000 (miss) |
| Small | 6,255 | 2,131 | < 2,000 (miss) |

Halving the parameters leaves the AVX2 step time unchanged, which suggests the matrix-vector work is not the bottleneck. The attention loop runs layers x heads x window short dot products, each ending in a horizontal reduction: 384 for the small model and 256 for the base. Computing a head's scores 8 window slots at a time is the next change to try; it has not been profiled. A forecast-only step that skips the generator head (100 x d_model) is written with a test but not yet built or measured.

## Not built

- int8 and AVX-VNNI paths, ONNX Runtime and LibTorch baselines (Stretch).
- Inference cost in the backtest's processing latency, stale-event policy, PnL against model latency.
- Quantisation loss in IC and PnL (no int8 path).
