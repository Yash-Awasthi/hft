# Roadmap

Each workstream has an end result that can be checked, a stretch goal that may not be
reachable, and a research gate. The gate is a short experiment that decides how much of the
stretch goal ships. Nothing past a gate starts before the gate has a written result.

Sizes: S is days, M is a few weeks, L is a month or more.

## 0. Open work from STATUS.md

Blocked on the repository owner (STATUS.md "Needs you"):

| Item | Decision or action |
|---|---|
| 2 | Hardware counters: the WSL2 guest still has no PMU with `hardwarePerformanceCounters=true`. Run the timed benchmarks once on bare Linux, where `perf stat` works. |
| 3 | Register the self-hosted nightly runner. |
| 6 | QQQ weights and fee schedules (regression default in use). |
| 8 | Unlock and run the test set once at a frozen tag. |
| 9 | Publish the pre-registration, and where. |
| 10 | M3 checkpoint licence decision. |

Unblocked, in order of value:

1. Small-tick Avellaneda-Stoikov quotes nothing (the 20-tick collar rejects the fitted
   distance). Result: a quoting rule that produces fills on small-tick stocks, or a written
   reason it cannot. S.
2. Method C fails validation at the current tick (INTC: 133 simulated moves against 7,241
   real). Result: simulator matches real move counts within 2x, or the method is removed
   from the forecast. M.
3. Not built: deep queue reservation, portfolio inventory, mechanical/reactive impact split.
   Result: each either built with a test or struck from DESIGN.md. M.
4. Model size miss: 21k parameters against a 10k target for the base model. Result: target
   restated or model shrunk with the IC reported. S.
5. Regenerate the LZ4 stores for the other three days with `transcode`; refresh STATUS.md
   after each workstream below. S.

## 1. Counterfactual branching market maker

The exchange state forks in 47 microseconds (one book) and 1.4 ms for a 50-symbol process
fork. At each quote decision the strategy forks the state, replays the next H milliseconds of
real feed against each of K candidate quote sets, and quotes the best.

End result: strategy `branch_mm` in `hftpy.backtest`, decision latency reported, PnL per
stock-day with confidence intervals against `dp_signal` on the same validation days, and
a test that rollouts are deterministic and leave the parent state untouched.

Research gate (M): rollouts replay real flow, so they ignore the market's reaction to our
quotes. Measure the bias: on held-out orders, compare predicted fill probability and
markout from rollouts with what actually happened (using the MS5 fill model as the
reference). Ship the full strategy if the bias is below the fill model's own error;
otherwise ship rollouts as a feature fed to the existing dynamic-programming policy.

Stretch (far-fetched): use branching as a teacher. Distil its choices into the 1.3 microsecond
transformer so the live decision costs one forward pass and no rollouts. Result if reached:
student recovers most of the teacher's PnL at about 1 microsecond per decision. L.

## 2. Prediction-market venue

Run the same book, engine and queue tracking on binary event contracts (prices in [0, 1],
payout at resolution) from a Polymarket-style central limit order book.

End result: a recorded multi-week dataset of order book snapshots and trades for a fixed
set of markets, written with a hand-written HTTP and WebSocket client (no third-party
code); a venue adapter that replays it through the existing engine; a market maker that
quotes in logit space with inventory and resolution-time risk; an arbitrage scanner for
YES + NO != 1 and for logically linked markets; one report in `docs/results/`.

Research gate (M): is there enough depth and fill data to calibrate anything? Record two
weeks, then measure spread, depth, trade rate and resolution clustering per market. Go on
if at least 20 markets have a median daily trade count that supports a fill model; otherwise
limit the work to the scanner.

Stretch (far-fetched): solve cross-market consistency as a linear program. Find the
cheapest set of trades that locks in a profit across a set of related contracts, with fees
and depth as constraints, and report how often and how large such sets are. A related
second step is an event contract hedged against its equity or ETF proxy. L.

## 4. Showcase

End result: one command, `scripts/showcase.sh`, builds a static site from the registry and
`docs/results/`, published on GitHub Pages, no external scripts or fonts. Pages:

- latency histograms for book update, decode and transformer step, with the machine and
  clock stated;
- the PnL identity and determinism checks for a chosen run;
- queue-position tracking error against the exact queue;
- regime and signal plots (IC by horizon, spread and depth by regime);
- a replay viewer: scrub through one stock's day and see book, our quotes and fills.

Research gate (S): can the engine build for the browser with the compiler already used
(WebAssembly target, no libraries)? If yes, the replay viewer runs the real engine
client-side; if not, it plays a precomputed trace of the same data.

Stretch (far-fetched): in the viewer, fork the state at any moment and let the visitor place
a quote, then see the branching strategy's rollouts for it (needs workstream 1). M after 1.

## 5. Linux performance track (supports all of the above)

- Run `perf stat` and the benchmark suite on bare Linux with isolated cores
  (`isolcpus`, `nohz_full`), fixed frequency, and record the setup in the results.
- Adopt PGO in the release build if CI time allows (measured: per-symbol replay -10%).
- Try, one at a time and keep only what measures better: `SCHED_FIFO`, `mlockall`,
  pinning the read-ahead and replay threads on separate cores, a reader that submits chunk
  reads ahead of decompression. Dropped already: order-ID prefetch on the per-symbol path.

End result: a table of before and after per change on the same machine, with counters
(instructions, cache misses, branch misses per event).

## Order

1. Section 0 items 1 and 5 and section 5 baseline on bare Linux (S).
2. Workstream 4 skeleton on current results, so every later result lands on a page (S).
3. Workstream 1 gate, then build (M to L).
4. Workstream 2 recording starts now in the background, since the gate needs two weeks of
   data; build after the gate (M to L).
5. Stretch goals in the order the gates allow.

Each workstream ends with an update to STATUS.md and its results page.
