# hft

An L3 market-making lab in C++23 on real Nasdaq TotalView-ITCH data: a zero-allocation
decoder and per-symbol compressed store, an order-by-order book, a deterministic matching
engine whose state forks with one `memcpy`, an event-driven backtester that queues
simulated orders inside the real queues, and the research built on top. Full design and
plan: [DESIGN.md](DESIGN.md); working log: [STATUS.md](STATUS.md).

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

## Performance

Measured on an i7-13650HX under WSL2, pinned, mains power, turbo off (about 2.4 GHz),
2026-10-07. Raw output: `book_study` and `hft_bench` CSVs, summarised by
`research/book_study.py` with bootstrap intervals and Mann-Whitney tests. An optimisation
pass (sequence-window merge, parallel read-ahead, one ID probe per book operation) has
landed since; its before/after figures are pending.

| Path | Target | Baseline |
|---|---|---|
| Book update, one symbol (GOOGL) | p50 ≤ 30 ns, p99 ≤ 150 ns | 53.5 / 152 ns |
| Book update, 50 symbols interleaved | p50 ≤ 30 ns, p99 ≤ 150 ns | 81.0 / 366 ns |
| Read + decode, 50 symbols | ≥ 50M msg/s | 18.8M decode thread alone, 17.8M pipeline |
| Full-day replay, 50 stocks | ≤ 60 s | 25.9 s |
| Fork, 10k orders | ≤ 100 µs, ≥ 10 GB/s | 53.4 µs, 16.0 GiB/s |
| Transformer step, AVX2 | < 2 µs | 3,435 ns (base), 2,935 ns (small) |

Book variants (`std::map`, Robin Hood and direct-mapped ID maps, AoS / SoA / hot-cold
layouts, sorted vector, B-tree) are compared in
[docs/results/ms2-book-study.md](docs/results/ms2-book-study.md), with instructions and
cache misses per event from Cachegrind.

## Correctness

- Decoder fields identical to the third-party itchfeed parser on 12M messages; merged store
  stream byte-identical to the decompressed download (SHA-256).
- Seven book variants give the same BBO stream as `std::map` on full days (353M and 650M
  messages), and an independent Python reference book agrees on 201 sampled symbols.
- Invariants checked on every event of 255M book events; zero heap allocations in steady
  state (`alloc_test`); checkpoints replay to byte-identical state.
- Matching engine against a naive reference engine (RapidCheck); golden BBO and engine
  hashes equal under GCC 13 / 15 and Clang 18 / 21.
- Feature pipeline: batch equals streaming bit for bit, and features are unchanged when the
  future is perturbed (leakage test).
- Real orders re-inserted as virtual orders receive the same executions (96.7% under the
  queue rule, 99.6% conservative).

## Tests and CI

```
ctest --test-dir build/debug --output-on-failure      # C++ (GoogleTest, RapidCheck)
python research/tests/test_backtests.py               # one of 15 Python suites
```

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
