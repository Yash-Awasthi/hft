# hft

An L3 market-making lab in C++23 that replays real Nasdaq TotalView-ITCH days through an
order-by-order book, a deterministic matching engine and an in-loop AVX2 transformer.

Full design and plan: [DESIGN.md](DESIGN.md); working log: [STATUS.md](STATUS.md).

## Architecture

```
 ITCH day (.gz) ──ingest──> per-symbol zstd store + index + book checkpoints
                                   │
                    MergedReader (parallel read-ahead, feed order restored)
                                   │
          ┌──────── Scheduler: (time, kind, insertion) ────────┐
          │                                                     │
   L3 book per symbol  <──  ReplayExchange: virtual orders  <── Strategy
   (TickBook)               in the real queues, fill rules      features -> model -> policy
          │                                                     -> risk
   MatchingEngine (price-time, IOC / post-only, STP, fees, half-penny tick)
          │
   Accounting (integer micro-dollars, PnL identity checked every event)
          │
   Experiment layer: TOML configs, process pool, SQLite registry, locked test days
          │
   nanobind module `hftpy` -> Python research (research/)
```

| Component | Where | Notes |
|---|---|---|
| ITCH decoder | `src/feed/itch.hpp` | Zero-copy, fuzzed (`fuzz/`) |
| Store | `src/data/store.*` | zstd chunks per symbol, global sequence numbers; merged back in feed order by a sequence-window merge with N decompression threads |
| Book | `src/book/tick_book.hpp` | 2,048-tick window with a two-level bitmap, radix tree for deep levels, off-grid array; hot/cold order pools addressed by 32-bit index; open-addressing order-ID map |
| Matching engine | `src/engine/matching.hpp` | Price-time; property-tested against a naive reference |
| Replay exchange | `src/engine/replay_exchange.hpp` | Virtual orders in real queues; queue, trade-through and hidden-print fill rules |
| Backtest | `src/backtest/` | Event-driven; zero, random, foresight, naive, Avellaneda-Stoikov (GLFT), DP and DP-with-signal strategies |
| Transformer step | `src/strategy/event_transformer.hpp` | Hand-written AVX2 float32 kernel, matched against PyTorch |
| Python | `bindings/hftpy.cpp`, `research/` | Features, fills, quoting, regime, impact, validity studies |

Every piece of engine state lives in index-addressed pools, so a book or engine fork is a
copy of the used part of each pool with no pointer fix-up.

**Threads.** Replay is single-threaded and deterministic: one thread merges the symbols back
into feed order, decodes, updates the books and runs the strategy. Zstd chunks are
decompressed ahead of it on N read-ahead threads and handed over strictly in sequence order,
so the merged stream stays byte-identical to the download. Parameter sweeps run whole
replays in parallel in a process pool.

**Book.** Price levels live in a 2,048-tick window around the mid with a two-level bitmap of
non-empty levels (the next level is one `tzcnt` / `lzcnt` per word); deeper levels go to a
radix tree and off-grid prices to a separate array. Orders are split into 16-byte hot and
24-byte cold records, and order IDs resolve through an open-addressing map with 8-byte
slots, probed once per operation.

**Transformer.** A small event transformer (d_model 24, 2 layers, 3 heads, sliding window
64 with an ALiBi bias, 10,618 parameters, 41.5 KB in float32) forecasts short-horizon price
moves from the event stream. The step is a hand-written AVX2 kernel: keys stored transposed
so scores take 8 window positions per FMA, independent FMA chains in the score and
attention loops, and a scalar reference path for parity checks.

## Performance

Measured on an i7-13650HX under WSL2, pinned, mains power, turbo off (about 2.4 GHz, clock
read before and after every run), 10 repetitions after one warm-up, medians with 95%
bootstrap intervals; before/after differences tested with Mann-Whitney. Cycle figures are
given where the reports derive them.

Targets were revised once, after the baseline and one optimisation pass. The original
targets were set from runs with turbo on (about 4.7 GHz); the baseline fixes the clock at
about 2.4 GHz so before/after comparisons are stable, which roughly doubles every time figure
for the same cycles. Old and new targets with the reason for each are in the revision record,
[DESIGN.md section 9, "Target revisions"](DESIGN.md#target-revisions).

| Path | Target | Baseline | Now | Cycles (now) | Source |
|---|---|---|---|---|---|
| Book update, one symbol (GOOGL), p50 | ≤ 55 ns | 53.5 ns | 52.8 ns | 127 | [ms2](docs/results/ms2-book-study.md) |
| Book update, one symbol (GOOGL), p99 | ≤ 160 ns | 152 ns | 136 to 159 ns | 336 (at 139.8 ns) | [ms2](docs/results/ms2-book-study.md) |
| Book update, 50 symbols interleaved, p50 / p99 | reported, no target | 81.0 / 366 ns | 79.6 / 367 ns | | [ms2](docs/results/ms2-book-study.md) |
| Read + decode, 50 symbols | ≥ 30M msg/s | 17.8M | 31.6M (two decompression threads) | 74 per message, decode thread alone (32.3M) | [ms2](docs/results/ms2-book-study.md) |
| Full-day replay, 50 stocks, one replay thread | ≤ 60 s | 25.9 s | 23.9 s | | [ms2](docs/results/ms2-book-study.md) |
| Fork, 10k orders | ≤ 100 µs, ≥ 10 GB/s | 53.4 µs, 16.0 GiB/s | not re-measured | | [ms2](docs/results/ms2-book-study.md) |
| Transformer forecast step, small model, AVX2 | < 2 µs | 2,786 ns | 1,975 ns | | [ms9](docs/results/ms9-transformer.md) |
| Transformer full step, small model, AVX2 | reported, no target | 2,935 ns | 2,140 ns | | [ms9](docs/results/ms9-transformer.md) |
| Transformer full step, base model, AVX2 | reported, no target | 3,435 ns | 2,545 ns | | [ms9](docs/results/ms9-transformer.md) |

The single-symbol p99 moves between runs (152 ns at the baseline, 175 ns in one later run),
so it is given as the 95% interval after the optimisation pass; the median is stable. The
base model has twice the parameters of the small one and is reported beside it.

What moved the numbers:

- **Merge.** Records from all symbols are scattered into a 4,096-sequence window and emitted
  by a bitmap scan instead of one heap operation per record: the decode thread went from
  23.8M to 32.3M msg/s.
- **Read-ahead.** Chunks decompress on N threads and are handed over strictly in sequence
  order, so output stays byte-identical (store SHA-256 unchanged): the pipeline went from
  18.1M to 31.6M msg/s with two threads.
- **Book.** One order-ID probe per operation instead of two to four, keeping the hash table
  image identical so stored checkpoints still verify: 7.5% off the single-symbol batch cost.
- **Transformer.** Score and attention loops rebuilt around independent FMA chains, plus
  AVX2 for the remaining scalar loops: 23 to 30% off the step.

Book variants (`std::map`, Robin Hood and direct-mapped ID maps, AoS / SoA / hot-cold
layouts, sorted vector, B-tree) are compared in
[docs/results/ms2-book-study.md](docs/results/ms2-book-study.md), with instructions and
cache misses per event from Cachegrind.


## Tests

The C++ suite has 216 GoogleTest and RapidCheck tests; 215 pass and one
(`PerfCounters.CountsInstructionsWhenAvailable`) skips where the machine exposes no hardware
performance counters. Fifteen Python suites cover the research code against the built module.

- **Golden hashes.** A synthetic fixture is regenerated byte for byte, and its golden BBO
  and matching-engine hashes are equal under GCC 13 / 15 and Clang 18 / 21.
- **PyTorch parity.** The AVX2 and scalar transformer kernels are checked against PyTorch:
  on 200,000 INTC validation-day events with trained weights the maximum absolute logit
  error is 1.4e-5 (small model) and 7.6e-6 (base), with forecast decisions equal on 100% of
  events. A golden test in CI (`EventTransformer.MatchesPyTorchAndAgreesOnDecisions`)
  requires error below 2e-5 and identical decisions on both paths.
- Decoder fields identical to the third-party itchfeed parser on 12M messages; merged store
  stream byte-identical to the decompressed download (SHA-256).
- Seven book variants give the same BBO stream as `std::map` on full days (353M and 650M
  messages), and an independent Python reference book agrees on 201 sampled symbols.
- Invariants checked on every event of 255M book events; zero heap allocations in steady
  state (`alloc_test`); checkpoints replay to byte-identical state.
- Matching engine against a naive reference engine (RapidCheck).
- Feature pipeline: batch equals streaming bit for bit, and features are unchanged when the
  future is perturbed (leakage test).
- Real orders re-inserted as virtual orders receive the same executions (96.7% under the
  queue rule, 99.6% conservative).

```
ctest --test-dir build/debug --output-on-failure      # C++ (GoogleTest, RapidCheck)
python research/tests/test_backtests.py               # one of 15 Python suites
```

## CI

GitHub Actions (`.github/workflows/ci.yml`) builds and tests GCC and Clang across the
`debug`, `release`, `asan`, `ubsan` and `tsan` presets, runs the benchmarks once as a smoke
test, fuzzes the decoder for 30 s, and runs the Python suites against the built module.
`scripts/nightly.sh` holds the full-day checks, for a self-hosted runner (not yet registered).

## Build and run

Requires CMake, Ninja, a C++23 compiler and `VCPKG_ROOT` pointing at a vcpkg checkout.

```
cmake --preset release
cmake --build build/release
build/release/tests/hft_tests
```

Presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `fuzz`, `pgo-gen`, `pgo-use`.

Day files are listed in `configs/splits.toml`. Download, verify and ingest one day into the
per-symbol store with:

```
scripts/ingest_day.sh S121225-v50.txt.gz
```

`ingest <store-dir> <day.gz>` decompresses in-process, checks the gzip CRC and records the
SHA-256 of the download in `<store-dir>/source.sha256`. Then:

```
store_cat <store> [threads] | sha256sum          # merged stream, equals the download
book_replay <store> tick                         # BBO hash and invariants, every symbol
checkpoint <store> [--verify]                    # write or verify book checkpoints
book_study <store> book 50 10                    # book update latency, 50 busiest symbols
book_study <store> replay 50 10 [threads]        # full-day replay
python research/backtests.py <py-build> --name <sweep> --days <YYYY-MM-DD>...
```

`research/count_itch.c` is an independent message counter used to cross-check the decoder.

## Research

Each milestone has a report under `docs/results/`:

- [Signals](docs/results/ms4-signals.md): streaming features, IC and decay; best combined
  10 ms IC 0.41 (large-tick) and 0.21 (small-tick) on the validation day.
- [Fills](docs/results/ms5-fills.md): competing-risks fill model calibrated on held-out
  orders, queue value, markouts.
- [Backtest](docs/results/ms6-backtest.md): strategies with fee and fill-rule sensitivity;
  no strategy is above zero with confidence after costs.
- [Regime](docs/results/ms7-regime.md): forecast of half-penny tick and fee-cap effects,
  checked against the Tick Size Pilot.
- [Impact](docs/results/ms8-impact.md): square-root exponent 0.45 from sign-run
  metaorders; propagator kernel with a no-arbitrage check.
- [Transformer](docs/results/ms9-transformer.md): event transformer in the trading loop.
- [Validity](docs/results/ms10-validity.md): metamorphic relations for replay, a
  queue-reactive simulator and the transformer.

A pre-registration draft is in `docs/prereg/`; test days stay locked until a frozen run.

## License

MIT; see [LICENSE](LICENSE).
