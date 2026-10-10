# Execution layer: index

Read order: INDEX -> DECISIONS -> PHASES (current step) -> ARCH/DESIGN/SECURITY sections the step cites.
Terse reference docs; ids are stable, cite them in commits and code comments where useful.

## Status

- Current: not started. Next step: `E0.1` (PHASES.md).
- Branch: `exec-layer`, created from `phase1-hotpath` at E0 start.
- Baseline hashes (must stay equal unless a step says otherwise):
  - `pm_live --replay ~/data/pm-live/20261009T160027Z` -> `decision_hash 6facf575ad8d84bc`
  - `pm_stats ~/data/pm` output -> keep a copy at E0 as `~/data/ref/pm_stats.tsv`

## Files

| File | Holds | Ids |
|---|---|---|
| DECISIONS.md | fixed choices, owner answers, open questions | D*, Q* |
| PHASES.md | phases, steps, checks, gates, status | E0.1 .. E7.n, G* |
| ARCH.md | modules, threads, data flow, record format, file map | A* |
| DESIGN.md | types, state machine, risk checks, ledger, sim model, arb algorithm | T*, S*, R*, L*, V*, X* |
| SECURITY.md | trust boundaries, rules, grep gates | SEC* |

## Conventions

- Units: Px int32 1e-4 (0..10000). Qty and Usd units fixed at E0 (D5). No double outside the maker model.
- Hot path rule: no alloc, lock, syscall, file I/O on the trading thread in steady state.
- Every step: inspect -> failing test -> code -> test -> measure if perf-relevant -> commit.
- Commit message: normal prose subject+body, max 3 lines, no attribution lines.
- Build: `wsl.exe -e bash -lc 'cd ~/hft && export VCPKG_ROOT=~/vcpkg && cmake --build build/<preset>'`.
- Presets to keep green: release, native, asan, ubsan, tsan, clang (+ fuzz targets).
- Edit via `\\wsl.localhost\Ubuntu\home\yash\hft\...`; multi-line scripts as files in `\\wsl.localhost\Ubuntu\tmp\`.
