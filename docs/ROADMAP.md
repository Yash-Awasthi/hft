# Roadmap

The system has two jobs: replay Nasdaq ITCH days as fast as the hardware allows, and run a
live paper-trading pipeline against a prediction-market exchange. Every change is measured
before and after, and every phase ends with CI green on all presets.

Target machine: i7-13650HX (Raptor Lake, 6 P-cores and 8 E-cores, AVX2, FMA, BMI2,
AVX-VNNI, GFNI; no AVX-512) under WSL2. WSL2 has no hardware performance counters, so
cycle counts come from Cachegrind and `rdtscp`.

## P0. Recorder robustness (done)

- A peer reset killed `pm_record` with SIGPIPE; the TLS client now ignores it.

## P1. Debloat (done)

- Research code, the experiment registry, the transformer and study apps moved to `archive/`.
- Removed from the build: tlx, toml++, SQLite, nanobind, the Python CI job.

## P2. Build for the machine

- `native` preset: `-O3 -march=native -flto`, combined with the profile-guided presets.
- Hot threads pinned to a P-core; E-cores left to the OS and decompression.
- Result: before/after for `book_study` and `pm_stats` on the same store.

## P3. Low-level hot paths

- Prediction-market book: flat price array over the 0.001 grid, bitmap of non-empty levels,
  integer sizes, in place of two `std::map`s.
- JSON: AVX2 structural scan (quotes, backslashes, braces found 32 bytes at a time) under the
  existing reader, checked against it on the recorded data.
- Single-producer single-consumer ring between the feed thread and the trading thread.
- Log-linear latency histogram in place of HdrHistogram.
- Result: messages per second for `pm_stats` and per-message parse cost, before and after.

## P4. Live paper-trading pipeline

```
 feed thread: TLS WebSocket -> parse -> ring -> trading thread: book -> strategy -> risk
                                                                   -> paper order manager
 telemetry: per-stage latency histograms, counters -> metrics endpoint + dashboard
```

- `pm_live`: subscribes to the same markets as the recorder, keeps books, runs the logit
  Avellaneda-Stoikov maker, and fills its paper orders against the live trade stream with
  a queue-ahead count. No real orders are sent.
- Risk: per-market and total position limits, a loss stop, a stale-feed stop.
- Telemetry: wire-to-book and book-to-decision latency, message rates, reconnects, PnL,
  served as plain text over HTTP on localhost and written to a status file.
- Record and replay: `pm_live --replay <dir>` runs the same code on recorded hours and must
  produce identical decisions for identical input.
- Arbitrage scanner: for markets whose outcomes are mutually exclusive, report when the
  best asks sum below 1 or the best bids sum above 1, with size and duration.

## Not planned

- Real order entry: needs signing keys and an account, which are the owner's decision.
- Bloomberg, FRED, Tiingo, Quiver: daily research data with no use in this pipeline.
