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

## In progress

- Recorder running since 2026-10-09 under the logon task, 32 markets (64 tokens), hourly zstd
  files in `$HFT_DATA/pm`, 50 GB cap.

## Needs the owner

- Hardware counters: the WSL2 guest has no PMU; timed runs with `perf stat` need bare Linux.
- Real order entry is out of scope until there is an account and a decision to trade.
