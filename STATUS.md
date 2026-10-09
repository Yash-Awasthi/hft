# Status

Working log for [docs/ROADMAP.md](docs/ROADMAP.md). The log of the research phase is in
[archive/docs/STATUS-research.md](archive/docs/STATUS-research.md).

## Done

- P0: `pm_record` no longer dies with SIGPIPE when a peer resets the connection (two exits
  with code 141 on 2026-10-09).
- P1: research code, experiment registry, transformer and study apps moved to `archive/`;
  tlx, toml++, SQLite and nanobind dropped from the build.

## In progress

- Recorder running since 2026-10-09 under the logon task, 32 markets (64 tokens), hourly zstd
  files in `$HFT_DATA/pm`, 50 GB cap.

## Needs the owner

- Hardware counters: the WSL2 guest has no PMU; timed runs with `perf stat` need bare Linux.
- Real order entry is out of scope until there is an account and a decision to trade.
