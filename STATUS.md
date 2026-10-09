# Status

Open work for [docs/ROADMAP.md](docs/ROADMAP.md). Finished work is in the git history and in
README "Performance"; the research-phase log is in
[archive/docs/STATUS-research.md](archive/docs/STATUS-research.md).

## Running

- Recorder (`pm_record`), started by the Windows logon task: 32 markets (64 tokens), hourly
  zstd files in `~/data/pm`, 50 GB cap, health in `status.json`.
- `pm_live` under `scripts/pm_live_supervise.sh ~/data/pm-live --record-cap-gb 20 --port 8088
  --seconds 21600`, started by hand (a reboot stops it). Dashboard at http://127.0.0.1:8088/.
  To stop it, kill the supervisor first, then `pm_live`.

## Open

1. The ITCH trading stack has no program: `src/backtest`, `src/strategy`, the matching
   engine and the replay exchange are reached only by tests since the Python module was
   archived. Either add an `itch_backtest` app (strategy, day, PnL, fills, latency) or move
   the stack to `archive/`.
2. Research-only headers still in the build, used by tests alone: `strategy/labeler.hpp`,
   `strategy/grid.hpp`, `strategy/qr_events.hpp`, `sources/queue_reactive.hpp`. Decide
   with item 1.
3. After 2 to 3 days of `pm_live` recordings: report arbitrage windows (count, duration,
   edge, size) and paper fills per event; an offline `pm_arb` over the recordings.
4. After about two weeks of recorder data: `pm_stats` and `pm_mm` over the full recording.
5. `pm_live` has no test for its session file and hourly rotation (both checked by hand).

## Needs the owner

- A Windows logon task for `pm_live`, like the recorder's, so it survives a reboot.
- A bare-Linux run for hardware counters and futex wake latency (WSL2: about 135 µs).
- Real order entry: an account, signing keys and the decision to trade.
- Hosting for a results page, if one is wanted.
