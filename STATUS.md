# Status

Open work for [docs/ROADMAP.md](docs/ROADMAP.md). Finished work is in the git history and in
README "Performance"; the research-phase log is in
[archive/docs/STATUS-research.md](archive/docs/STATUS-research.md).

## Running

Nothing. Recorder and `pm_live` were stopped on 2026-10-10 and nothing starts at logon
(`C:\Users\Yash\hft\pm_start.ps1` does nothing; `pm_start.ps1.on` is the old version).
Their recordings stay in `~/data/pm` and `~/data/pm-live`. To run them by hand:

    scripts/pm_supervise.sh ~/data/pm --cap-gb 50 --per-tag 8
    scripts/pm_live_supervise.sh ~/data/pm-live --record-cap-gb 20 --port 8088 --seconds 21600

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
