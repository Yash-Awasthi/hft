# hft

[![ci](https://github.com/Yash-Awasthi/hft/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Yash-Awasthi/hft/actions/workflows/ci.yml)

A low-latency trading system in C++23 with no framework underneath. It has two feeds:

- **Nasdaq TotalView-ITCH**, replayed from disk through an order-by-order book, a
  deterministic matching engine and an event-driven backtest.
- **A live prediction-market exchange** (Polymarket's public order-book stream), read by a
  hand-written TLS WebSocket client, recorded to disk and rebuilt into books.

Plan and phases: [docs/ROADMAP.md](docs/ROADMAP.md); working log: [STATUS.md](STATUS.md).
Studies from the earlier research phase (Python, signal and regime reports, transformer
forecaster, pre-registration) are kept out of the build under [archive/](archive/).

## Architecture

```
 ITCH day (.gz) --ingest--> per-symbol LZ4/zstd store + index + book checkpoints
                                  |
                   per-symbol replay on a thread pool / merged reader
                                  |
   L3 book per symbol (TickBook) <- ReplayExchange: virtual orders <- strategy
                                  |                                   (AS / GLFT, DP)
   MatchingEngine (price-time, IOC / post-only, STP, fees, half-penny tick)
                                  |
   Accounting (integer micro-dollars, PnL identity checked every event)

 Polymarket WS --TLS--> pm_record --> hourly zstd JSONL --> pm_stats / pm_mm

 pm_live:  feed thread per connection: TLS WebSocket -> receive stamp -> SPSC byte ring
           trading thread: tape parse -> books -> makers (paper fills) -> risk -> arb scanner
           recording thread (replayable), HTTP thread: /metrics, /metrics.json, dashboard
```

| Component | Where | Notes |
|---|---|---|
| ITCH decoder | `src/feed/itch.hpp` | Zero-copy, fuzzed (`fuzz/`) |
| Store | `src/data/store.*` | LZ4-HC or zstd chunks per symbol, global sequence numbers, sequence-window merge with N decompression threads |
| Book | `src/book/tick_book.hpp` | 2,048-tick window with a two-level bitmap, radix tree for deep levels, off-grid array; hot/cold order pools addressed by 32-bit index; open-addressing order-ID map |
| Matching engine | `src/engine/matching.hpp` | Price-time; property-tested against a naive reference |
| Replay exchange | `src/engine/replay_exchange.hpp` | Virtual orders in real queues; queue, trade-through and hidden-print fill rules |
| Backtest | `src/backtest/` | Event-driven; zero, random, foresight, naive, Avellaneda-Stoikov (GLFT), DP strategies |
| Network | `src/net/` | TLS socket (OpenSSL for the cipher layer only), HTTP/1.1 GET, WebSocket framing, SHA-1, base64; tape JSON parser with an AVX2 first stage |
| Prediction markets | `src/pm/`, `apps/pm_*.cpp` | Recorder, typed message decoder, flat bitmap book, book rebuild, logit Avellaneda-Stoikov maker |
| Core | `src/core/` | Index-addressed pools, SPSC ring, log-linear histogram, `rdtscp` timing, Philox RNG |

Every piece of engine state lives in index-addressed pools, so a book or engine fork is a
copy of the used part of each pool with no pointer fix-up. Replay is single-threaded and
deterministic per symbol; zstd/LZ4 chunks are decompressed ahead of it and handed over in
sequence order, so the merged stream stays byte-identical to the download.

Third-party code at run time: zstd, LZ4 and zlib-ng for storage, OpenSSL for TLS.
GoogleTest, RapidCheck and Google Benchmark are used by tests and benchmarks only.

## Performance

Measured on an i7-13650HX under WSL2, pinned, mains power, turbo off (about 2.4 GHz, clock
read before and after every run), 10 repetitions after one warm-up, medians with 95%
bootstrap intervals; before/after differences tested with Mann-Whitney. Cycle figures are
given where the reports derive them.

Targets were revised once, after the baseline and one optimisation pass. The original
targets were set from runs with turbo on (about 4.7 GHz); the baseline fixes the clock at
about 2.4 GHz so before/after comparisons are stable, which roughly doubles every time figure
for the same cycles. Old and new targets with the reason for each are in the revision record,
[archive/docs/DESIGN.md section 9](archive/docs/DESIGN.md#target-revisions).

| Path | Target | Baseline | Now | Cycles (now) | Source |
|---|---|---|---|---|---|
| Book update, one symbol (GOOGL), p50 | ≤ 55 ns | 53.5 ns | 52.8 ns | 127 | [ms2](docs/results/ms2-book-study.md) |
| Book update, one symbol (GOOGL), p99 | ≤ 160 ns | 152 ns | 136 to 159 ns | 336 (at 139.8 ns) | [ms2](docs/results/ms2-book-study.md) |
| Book update, 50 symbols interleaved, p50 / p99 | reported, no target | 81.0 / 366 ns | 79.6 / 367 ns | | [ms2](docs/results/ms2-book-study.md) |
| Read + decode, 50 symbols | ≥ 30M msg/s | 17.8M | 31.6M (two decompression threads) | 74 per message, decode thread alone (32.3M) | [ms2](docs/results/ms2-book-study.md) |
| Full-day replay, 50 stocks, one replay thread | ≤ 60 s | 25.9 s | 23.9 s | | [ms2](docs/results/ms2-book-study.md) |
| Fork, 10k orders | ≤ 100 µs, ≥ 10 GB/s | 53.4 µs, 16.0 GiB/s | 46.9 µs, 17.2 GiB/s (one book; 50 books 4.95 ms by copy, 1.40 ms by process `fork()`) | | [ms2](docs/results/ms2-book-study.md) |

The single-symbol p99 moves between runs (152 ns at the baseline, 175 ns in one later run),
so it is given as the 95% interval after the optimisation pass; the median is stable.

What moved the numbers:

- **Merge.** Records from all symbols are scattered into a 4,096-sequence window and emitted
  by a bitmap scan instead of one heap operation per record: the decode thread went from
  23.8M to 32.3M msg/s.
- **Read-ahead.** Chunks decompress on N threads and are handed over strictly in sequence
  order, so output stays byte-identical (store SHA-256 unchanged): the pipeline went from
  18.1M to 31.6M msg/s with two threads.
- **Book.** One order-ID probe per operation instead of two to four, keeping the hash table
  image identical so stored checkpoints still verify: 7.5% off the single-symbol batch cost.

A later pass (replay lookahead, backtest hot paths, LZ4 store, per-symbol replay) was timed
back to back against the previous `main`, so these pairs are comparable with each other but not with the table above, whose clock drifted between sessions:

| Path | Before | After |
|---|---|---|
| Full-day replay, 50 stocks, 147.6M events | 24.1 s | 17.2 s |
| `book_replay`, all 12,076 symbols | 40.8 s | 7.5 s |
| `checkpoint` write / verify | 4.7 s / 2.4 s | 0.92 s / 0.66 s |

The merged-feed replay lookahead is used only by `bench/book_study.cpp`; the production
replays are per symbol.

Profile-guided build (`pgo-gen`, run `book_study replay` and `replay-sym`, then `pgo-use`): per-symbol
replay of the 50 busiest symbols 11.7 s to 10.6 s (-10%, two runs each, one thread); the merged
replay is unchanged within noise. A lookahead prefetch on the per-symbol path was slower
(10.5 s to 11.7 s) and was dropped.

## Tests

`ctest` runs about 215 GoogleTest and RapidCheck tests; one
(`PerfCounters.CountsInstructionsWhenAvailable`) skips where the machine exposes no hardware
performance counters.

- A synthetic fixture is regenerated byte for byte; its golden BBO and matching-engine hashes
  are equal under GCC 13 / 15 and Clang 18 / 21.
- Decoder fields are identical to the third-party itchfeed parser on 12M messages; the merged
  store stream is byte-identical to the decompressed download (SHA-256).
- The tick book gives the same BBO stream as `std::map` on full days (353M and 650M messages).
- Invariants are checked on every event of 255M book events; there are zero heap allocations in
  steady state (`alloc_test`); checkpoints replay to byte-identical state.
- The matching engine is checked against a naive reference engine (RapidCheck).
- WebSocket framing, SHA-1, base64 and JSON have known-answer tests.

CI (`.github/workflows/ci.yml`) builds and tests GCC and Clang across the `debug`,
`release`, `asan`, `ubsan` and `tsan` presets, runs the benchmarks once as a smoke test
and fuzzes the decoder for 30 s.

## Build and run

Requires CMake, Ninja, a C++23 compiler, OpenSSL development files and `VCPKG_ROOT` pointing
at a vcpkg checkout.

```
cmake --preset release
cmake --build build/release
ctest --test-dir build/release
```

Presets: `debug`, `release`, `native` (this CPU, LTO), `asan`, `ubsan`, `tsan`, `fuzz`, `pgo-gen`, `pgo-use`.

Ingest one ITCH day into a per-symbol store (download, verify, ingest):

```
scripts/ingest_day.sh S121225-v50.txt.gz
store_cat <store> [threads] | sha256sum          # merged stream, equals the download
book_replay <store> tick                         # BBO hash and invariants, every symbol (THREADS=N)
checkpoint <store> [--verify]                    # write or verify book checkpoints
book_study <store> book 50 10                    # book update latency, 50 busiest symbols
book_study <store> replay-sym 50 10 [threads]    # full-day replay, one symbol at a time on a pool
itch_backtest <store> INTC avellaneda_stoikov maker_rebate=2000 taker_fee=3000 max_dist_ticks=5
```

`itch_backtest` runs one strategy on one symbol-day: the strategy sees the market a
market-data latency late, its orders join the real queues after the order-entry latency,
fees and risk limits apply, and the PnL identity is checked on every event. Strategies:
`zero`, `naive`, `random_taker`, `random_passive`, `perfect_foresight`,
`avellaneda_stoikov`, `dp:<policy>`, `ext:<policy>`. INTC on 2025-12-10 (1.8M events)
runs in about 1.5 s.

`ingest <store-dir> <day.gz> [--codec lz4|zstd]` checks the gzip CRC and records the SHA-256
of the download. LZ4-HC decompresses 3.5 times faster than zstd for 11% more disk.

## Prediction markets

`pm_record` stores the public order-book stream: one line per message with the receive time,
in hourly zstd files. It picks the markets with the most volume per tag, reconnects with
backoff, logs every gap and stops at a size cap. `scripts/pm_supervise.sh <dir>` restarts it
after a crash; `status.json` in the record directory holds message counts, reconnects and the
age of the last message per connection.

```
pm_record --out data/pm --cap-gb 50 --per-tag 8
pm_stats data/pm > tokens.tsv     # per-token book rebuild: spread, depth, trades, consistency
pm_mm data/pm > mm.tsv            # logit Avellaneda-Stoikov maker on the recorded books
```

Both tools read a recording at about 1M messages per second: zstd decompression runs on a second
thread, a tape JSON parser with an AVX2 first stage feeds a typed decoder, and each token's book
is a flat price array with a two-level bitmap (on 2.0M messages, `pm_stats` went from 11.0 s and
469 MB to 1.9 s and 31 MB with identical output).

`pm_stats` checks its rebuilt book against the exchange's own best prices (0.03% of deltas
differ in a 20-minute sample). `pm_mm` fills quotes from recorded trades only, with a
queue-ahead count per order, so its fills are conservative.

## Live paper trading

`pm_live` runs the whole pipeline on the live stream and never sends an order:

```
pm_live --record data/live --seconds 3600          # dashboard on http://127.0.0.1:8088/
pm_live --record data/live --spin --cpu 4          # busy-poll a pinned core for latency
pm_live --replay data/live                         # same decisions, same hash
scripts/pm_live_supervise.sh data/pm-live --seconds 21600   # restart every 6 h, one directory per run
```

Recordings rotate hourly (zstd) and stop at `--record-cap-gb` (default 20) while trading
goes on. A stop signal exits with 128 plus the signal, which the supervisor treats as a stop.

It picks the most traded events in which exactly one market resolves Yes and every open
market trades, and subscribes to every token in them. Each connection has its own thread,
which stamps a message on receipt and copies it into a lock-free byte ring. The trading
thread drains the rings and passes each message through the tape parser and the books. A logit
Avellaneda-Stoikov maker runs per token, with paper fills from live trades behind a
queue-ahead count. Portfolio risk can halt the session on a loss stop, and pauses quoting
when gross inventory hits its cap or a connection goes silent. On a reconnect, books stay
invalid until a fresh snapshot arrives. The arbitrage scanner tracks every window in which
an event's Yes asks sum below 1 or its bids above 1, and does the same for each market's
Yes/No pair.

The trading core is a pure function of the (receive time, message) sequence. The recording
holds exactly that sequence in processing order, including heartbeats and the trading
thread's own clock records, so `--replay` reproduces the live decisions bit for bit (checked
by a hash of the decision log). `/metrics` is Prometheus text; `/metrics.json` feeds the
dashboard.

Latency on this laptop (WSL2, one 60 s session each, per message, receive stamp to decision):

| Mode | Ring queue p50 | Parse p50 | Books, strategy, risk p50 | Wire to decision p50 / p99 |
|---|---|---|---|---|
| default (futex doorbell) | 135 µs | 8.5 µs | 6.6 µs | 151 / 916 µs |
| `--spin --cpu 4` | 2.2 µs | 2.4 µs | 1.0 µs | 9.8 / 210 µs |

Under WSL2, waking an idle virtual CPU goes through the hypervisor, so the futex wake is
slow; the default trades latency for an idle core. Ring queueing in spin mode comes from
bursts: messages arrive back to back faster than they are processed.

## License

MIT; see [LICENSE](LICENSE).
