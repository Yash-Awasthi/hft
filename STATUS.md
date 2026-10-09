# Status

Working log for [docs/ROADMAP.md](docs/ROADMAP.md). The log of the research phase is in
[archive/docs/STATUS-research.md](archive/docs/STATUS-research.md).

## Done

- P0: `pm_record` no longer dies with SIGPIPE when a peer resets the connection (two exits
  with code 141 on 2026-10-09).
- P1: research code, experiment registry, transformer and study apps moved to `archive/`;
  tlx, toml++, SQLite and nanobind dropped from the build.
- P2: `native` preset (`-march=native -flto -ffp-contract=off`; the profile-guided presets use
  the same flags). All 216 tests pass. Per-symbol replay of the 50 busiest symbols on
  2025-12-10 (197.8M events, one thread, LZ4 store, turbo on, three alternating runs):
  14.20 s release, 13.73 s native (-3.3%). `-ffp-contract=off` keeps floating-point results
  equal to the generic build. Pinning inside WSL2 selects a virtual CPU; the host decides
  whether it runs on a P-core.

- P3: prediction-market path rebuilt for speed. On a fixed 2.0M-message recording (1.3 GB of
  JSON, 18 hour files), native build, best of two runs:

  | Tool | Before | After | Peak memory |
  |---|---|---|---|
  | `pm_stats` | 11.0 s | 1.93 s | 469 MB to 31 MB |
  | `pm_mm` | 10.8 s | 1.67 s | 469 MB to 32 MB |

  Outputs are identical to the old tools (row order among ties now breaks on the token id).
  What changed: a tape JSON parser (`src/net/tape.hpp`; AVX2 character classes, escaped
  quotes by the odd-backslash-run rule, string interiors by a carry-less multiply prefix xor,
  scalar starts emitted as tokens so stage 2 never scans blanks) that accepts exactly what
  the tree reader accepts; a typed message decoder that reads each object once; a flat
  10,001-slot book per side with a two-level bitmap; dense token indices; zstd decompression
  on a second thread behind a single-producer single-consumer ring. zstd alone takes about
  1.1 s of the remaining time on one core. HdrHistogram replaced by a 60-line log-linear
  histogram.

## In progress

- Recorder running since 2026-10-09 under the logon task, 32 markets (64 tokens), hourly zstd
  files in `$HFT_DATA/pm`, 50 GB cap.

## Needs the owner

- Hardware counters: the WSL2 guest has no PMU; timed runs with `perf stat` need bare Linux.
- Real order entry is out of scope until there is an account and a decision to trade.
