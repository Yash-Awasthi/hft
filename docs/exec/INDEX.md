# Execution layer: index

Read order: INDEX -> DECISIONS -> PHASES (current step) -> ARCH/DESIGN/SECURITY sections the step cites.
Terse reference docs; ids are stable, cite them in commits and code comments where useful.

## Status

- Current: E0-E2 done (G0-G2 met 2026-10-10). Next step: `E3.1` (V-impl in DESIGN).
- Branch: `exec-layer` (from `phase1-hotpath`).
- Baseline hashes (must stay equal unless a step says otherwise):
  - `pm_live --replay ~/data/pm-live/20261009T160027Z` -> `decision_hash 6facf575ad8d84bc`
  - `pm_stats ~/data/ref/pm-snap` (frozen copy, 44 files) -> `~/data/ref/pm_stats.tsv`, sha256 `dafd558b45dc3a1a...`

## Files

| File | Holds | Ids |
|---|---|---|
| DECISIONS.md | fixed choices, owner answers, open questions | D*, Q* |
| PHASES.md | phases, steps, checks, gates, status | E0.1 .. E7.n, G* |
| ARCH.md | modules, threads, data flow, record format, file map | A* |
| DESIGN.md | types, state machine, risk checks, ledger, sim model, arb algorithm | T*, S*, R*, L*, V*, X* |
| SECURITY.md | trust boundaries, rules, grep gates | SEC* |
| VENUE.md | exchange facts with sources, measured RTT and feed facts | F* |
| REFS.md | references mapped to components | RF* |
| HW.md | machine facts, measurement conditions | H* |
| PERF.md | build and runtime measurements, what was adopted and why not | P*, R* |

## Conventions

- Units (D5): Px int32 1e-4 USD; Qty int64 1e-6 share; Usd int64 1e-10 USD (Px*Qty exact). No double outside the maker model.
- Hot path rule: no alloc, lock, syscall, file I/O on the trading thread in steady state.
- Every step: inspect -> failing test -> code -> test -> measure if perf-relevant -> commit.
- Commit message: normal prose subject+body, max 3 lines, no attribution lines.
- Build: `wsl.exe -e bash -lc 'cd ~/hft && export VCPKG_ROOT=~/vcpkg && cmake --build build/<preset>'`.
- Presets to keep green: release, native, asan, ubsan, tsan, clang (+ fuzz targets).
- Edit via `\\wsl.localhost\Ubuntu\home\yash\hft\...`; multi-line scripts as files in `\\wsl.localhost\Ubuntu\tmp\`.
