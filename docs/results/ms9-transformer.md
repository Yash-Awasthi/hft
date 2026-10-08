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

## Step latency at the wall-clock baseline

| Model | Scalar median ns | AVX2 median ns | AVX2 forecast only | Target |
|---|---|---|---|---|
| Base | 14,180 | 3,435 | 3,205 | < 2,000 (miss) |
| Small | 9,598 | 2,935 | 2,786 | < 2,000 (miss) |

`hft_bench --benchmark_filter=EventStep --benchmark_repetitions=10` with `HFT_TRANSFORMER` pointing at the trained weights, pinned to one virtual CPU; window full. Idle machine on mains, turbo off (maximum processor state 99% for every core class, boost mode off; about 2.4 GHz). Standard deviations across repetitions are 9 to 40 ns.

## Small model

`python research/transformer.py ... --size small`: d_model 24, 2 layers, 3 heads (head size 8), MLP 24, window 64; 10,618 parameters (41.5 KB in float32), same data and 3,000 steps.

| Model | Params | Forecast CE | IC, 10 events | Direction accuracy, 10 events | Generator CE |
|---|---|---|---|---|---|
| Base | 21,354 | 0.884 | 0.409 | 75.5% | 2.568 |
| Small | 10,618 | 0.888 | 0.403 | 74.7% | 2.584 |

Kernel against PyTorch, 200,000 INTC validation events, with attention scores from the transposed keys: max abs logit error 1.6e-5 on both paths; same arg-max at 10 and 100 events and same next-event class on 100% of events for both paths. The base model: 7.2e-6 (AVX2), forecast decisions 100%, next-event class 99.9995%.

Step latency on an idle machine with turbo on (before the baseline), 10 repetitions, window full. The AVX2 path computes each head's scores 8 window positions per FMA from keys stored transposed, instead of one horizontal reduction per position; "before" is the previous kernel built from the parent commit and run back to back:

| Model | Scalar median ns | AVX2 before | AVX2 median ns | AVX2 forecast only | Target |
|---|---|---|---|---|---|
| Base | 7,699 | 1,925 | 1,841 | 1,725 | < 2,000 |
| Small | 5,191 | 1,867 | 1,537 | 1,459 | < 2,000 |

Standard deviations across repetitions are 6 to 18 ns. The earlier figures of about 2,130 ns for both models were taken while a backtest sweep ran on 4 to 6 cores; with turbo on (about 4.7 GHz) both kernels met the target, but not at the baseline above; the transposed scores take 4% (base) and 18% (small) off the step.

## Kernel optimisation (2026-10-08)

Same conditions as the baseline table (clock 2.39 to 2.45 GHz, read before and after every
run), 10 repetitions, before built from the parent commit (`ee49337`), after from `5ccbb38`.
The step was latency-bound: the scores and the attention sum each ran one long chain of
dependent FMAs. Changes, cumulative:

1. Scores four 8-slot blocks at a time, and the attention sum over the ring as two
   contiguous runs with four partial sums (base 3,329 to 2,856 ns, small 2,971 to 2,654).
2. Score scaling, ALiBi bias and maximum in AVX2 over the two runs; matrix-vector blocks of
   64 rows so each broadcast feeds eight FMAs (2,619 / 2,190).
3. The last one to three row blocks of a matrix-vector product share each broadcast (no
   measurable change, 2,595 / 2,174; kept as the tail of the same loop).
4. RMSNorm in AVX2, and the softmax's 1/sum applied once to the attention output instead of
   to every probability (2,545 / 2,140).

| Model | Path | Before ns | After ns | Change | Mann-Whitney p | Target |
|---|---|---|---|---|---|---|
| Base | AVX2 | 3,329 | 2,545 | -23.5% | 0.00018 | < 2,000 (miss) |
| Base | AVX2 forecast only | 3,127 | 2,373 | -24.1% | 0.00018 | |
| Small | AVX2 | 2,971 | 2,140 | -28.0% | 0.00018 | < 2,000 (miss) |
| Small | AVX2 forecast only | 2,827 | 1,975 | -30.1% | 0.00018 | < 2,000 (met) |

Standard deviations after are 11 to 74 ns. The step is now close to instruction-bound
(about 2.8 instructions per cycle; Cachegrind counts about 15k instructions per step for the
small model). Kernel against PyTorch on the same 200,000 INTC validation events after the
change: max abs logit error 7.6e-6 (base, AVX2) and 1.4e-5 (small, AVX2); forecast arg-max
equal on 100% of events at 10 and 100 events; next-event class 99.9995% (base) and 100%
(small). Raw output: `~/data/opt-2026-10-07/step-*.csv`.

## Not built

- int8 and AVX-VNNI paths, ONNX Runtime and LibTorch baselines (Stretch).
- Inference cost in the backtest's processing latency, stale-event policy, PnL against model latency.
- Quantisation loss in IC and PnL (no int8 path).
