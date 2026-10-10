# Status

Open work for [docs/ROADMAP.md](docs/ROADMAP.md). Finished work is in the git history and in
README "Performance"; the research-phase log is in
[archive/docs/STATUS-research.md](archive/docs/STATUS-research.md).

## Running

- Recorder (`pm_record`), started by the Windows logon task: 32 markets (64 tokens), hourly
  zstd files in `~/data/pm`, 50 GB cap, health in `status.json`.
- `pm_live` under `scripts/pm_live_supervise.sh ~/data/pm-live --record-cap-gb 20 --port 8088
  --seconds 21600`, started by the same logon task as the recorder:
  `C:\Users\Yash\hft\pm_start.ps1` hands off to `pm_start.vbs`, which starts WSL with no
  window (earlier versions: `pm_start.ps1.bak`, `.bak2`). Lock files keep each supervisor
  to one copy; the recorder's console output goes to `~/data/pm/console.log`. Dashboard at http://127.0.0.1:8088/. To stop it, kill the
  supervisor first, then `pm_live`.

## Open

1. After 2 to 3 days of `pm_live` recordings: `pm_arb ~/data/pm-live` and a short report.
   First 2 h (675k messages): Yes/No pair windows last under 0.1 ms and open both ways at
   once, which points at the two halves of one change arriving in separate messages, not
   at a tradeable price; event-level windows last seconds to minutes with gross edges up
   to 0.05 but a thinnest leg of about 8 to 70 shares. A filter for sub-millisecond windows
   and a check for stale legs are the next things to add.
2. After about two weeks of recorder data: `pm_stats` and `pm_mm` over the full recording.

## Needs the owner

- A bare-Linux run for hardware counters and futex wake latency (WSL2: about 135 µs).
- Real order entry: an account, signing keys and the decision to trade.
- Hosting for a results page, if one is wanted.
