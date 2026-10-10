# Phases

Each step: [ ] todo, [x] done (commit id). Gate Gn must pass before the next phase starts.
Step template: do -> check. "Ask" = stop and ask the owner before continuing.

## E0 Facts and inspection (no product code)

- [x] E0.1 (5f7aa4a; ~/data/ref/pm-snap frozen, 44 files, pm_stats sha256 dafd558b45dc3a1a; replay hash re-checked) Create branch `exec-layer` from `phase1-hotpath`. Save `pm_stats ~/data/pm` to `~/data/ref/pm_stats.tsv`; confirm baseline hash (INDEX).
- [x] E0.2 (2026-10-10, docs only) Read in full: engine/scheduler.hpp, backtest/risk.hpp, backtest/accounting.hpp, engine/matching.hpp, pm/maker.hpp, pm/engine.hpp, pm/arb.hpp, core/pool.hpp, book/id_map.hpp. Confirm or amend D8 (reuse map).
- [x] E0.3 (2026-10-10) Answer Q3..Q10 from the exchange's current public API docs; write `docs/exec/VENUE.md` (fact, source URL, date read). Fix D5 units.
- [x] E0.4 (2026-10-10) Measure RTT to the exchange API host: 20 TCP connects, median/p90 (Q11). Record in VENUE.md.
- [x] E0.5 (2026-10-10) Check recorded data for: tick_size_change events, market close/resolution events, min order size hints. Record counts in VENUE.md.
- [x] E0.6 (cf8b358) Data integrity: recorders ignore SIGHUP, pm_record exits 128+signal (was 0, so its supervisor restarted it), torn last lines dropped on compression (12 torn lines in 3.9M found in pm-snap, one with 4083 NUL bytes from an unclean WSL stop); CMake module scanning off (ninja 1.13 assertion).
- [x] E0.7 Hardware verified: HW.md.
- Ask if any fact contradicts DESIGN (e.g. fills not attributable to orders, no complete-set payout).
- G0: VENUE.md complete with dates; D5 and D8 final; DECISIONS updated. MET 2026-10-10.

## E1 Types, ledger, risk

- [x] E1.0 (see commit) Session file gains market category, neg_risk, min_order_size, tick (from Gamma/book) for D18, F8; recorded sessions without them get defaults (category Other, min 5, tick from book).
- [x] E1.1 (7c5aab7) `.gitignore` secret patterns (SEC7).
- [x] E1.2 (4e8012d) src/exec/types.hpp (T1..T7) + static_asserts.
- [x] E1.3 (4a04a29) Ledger tests first (L2 identity property, L4 settlement, L3 capital-days), then src/exec/ledger.hpp.
- [x] E1.4 Risk tests first: each R1..R8 boundary (limit pass, limit+1 step reject); token bucket refill; R9..R15 triggers; R16 trip blocks and cancels; then src/exec/risk.hpp.
- [x] E1.5 Bench: pre-trade check 27 ns mean at 2/40/200 tokens (native, vCPU 6; batch-timed mean, per-call p99 not measurable: timer > check). First version 270 ns at 200 tokens (group/gross loops) -> running sums in ledger.
- G1: tests pass all presets; bench within budget; mutation check (plant: R5 ignores open orders; R16 does not block) both caught. MET 2026-10-10 (release, native, asan, ubsan, tsan, clang; risk mutations 5/5, ledger 3/3, fee 1/1).

## E2 OMS

- [x] E2.1 (90f6ccf) Table test: every (state, event) pair -> expected state or S13.
- [x] E2.2 (90f6ccf) Reference model (naive, std::map) for property test.
- [x] E2.3 (90f6ccf) src/exec/oms.hpp: Pool order table, cl_id (T6), S1..S16, timeouts, Status/reconcile hook.
- [x] E2.4 (90f6ccf; 3 seeds x 100k) Property test 100k cases: random interleavings incl. dup/reorder/timeout vs reference: cum fill monotone, <= qty; open qty, reservations, positions equal reference.
- [x] E2.5 (b91f5d9) fuzz/oms_fuzz.cpp; 10 min clean (2.9M runs, ASan+UBSan, RSS flat 486 MB).
- [x] E2.6 (b91f5d9) alloc_test extended: steady-state submit/ack/fill/cancel zero allocations.
- [x] E2.7 (b91f5d9) Bench S17: 57 ns per 5-operation cycle.
- G2: all above; mutation check (plant: overfill accepted; dup fill counted twice; reject not releasing reservation) all caught. MET 2026-10-10 (4/4 OMS mutations caught).

## E3 SimVenue

- [x] E3.1 (see commit) Matching V1..V7 with unit tests on constructed books (taker walk, overlay clear, maker ahead, Fok/Fak/Gtc, fees, rule rejects).
- [x] E3.2 Invariants V15..V17 as always-on checks in tests and debug builds.
- [x] E3.3 Faults (D14, D16): core V18, V9, V10, V11, V14 with Philox, each forced on in a test; V8, V12, V13 optional.
- [x] E3.3b Venue rules: F8 min size, F10 delay, D18 fees tested in SimVenue; L6 merge/split in Ledger; D19 batches belong to the executor (E5). F6 rounding never applies to our orders: 2-dp sizes times tick-dp prices land exactly on the amount precision (3+2=5, 2+2=4, 4+2=6). F12 cancel-only mode not seen in data: deferred.
- Mutations: overlay off, ahead not capped, FOK check off, settlement ordered, compat side inverted: 5/5 caught. One release segfault during the mutation script did not reproduce (0/11, ASan and UBSan clean).
- [x] E3.0 (F32) Measure on pm-snap: share of last_trade_price where feed side disagrees with the side implied by price vs the book just before (D21, RF27). Record in VENUE.md.
- [x] E3.4 Parity G3b prep (pm_live --replay --dump-fills; ~/data/ref/maker_fills_old.txt): run maker on reference recording, old path, dump fills (ns, token, px, qty) to file.
- [x] E3.5 Maker quotes become intents (EngineParams.venue; pm_live --venue-compat) (A2.7); fills via SimVenue with latency 0, faults off; compare dump: must be identical.
- Ask if parity is not exact after one day of investigation (stop condition).
- G3: (a) invariants hold on full reference recording under each fault mode; (b) maker parity exact. MET 2026-10-10.
  (b) scripts/check_parity.sh: decision hash and fills identical on every recording, default and high-fill maker settings (10/10 frozen runs; the run being recorded differs only because it grows; its frozen copy matches). Mutation (queue ahead forced to 0): 14 -> 17 fills, hash differs.
  (a) scripts/fault_campaign.sh (50 ms latency, 20 ms jitter): no fault, drop ack 20%, duplicate 20%, settlement failure 20%, disconnect 8 s/120 s: 0 illegal reports, no kill; drop fill 20%: kill on position mismatch. Duplicates leave the decision hash unchanged.
  Found and fixed on the way: inbound requests reordered by jitter (a cancel overtook its order); Reject after an early cancel is legal; duplicate final reports and settlements for unseen fills are counted, not illegal; asynchronous fills were not logged; a dropped fill on an acked order went unnoticed until position reconciliation was added (R14).

## E4 pm_exec app

- [x] E4.1 apps/pm_exec.cpp: replay mode over recording + exec.cfg + seed; event loop A1. Shares the feed, trading loop, recorder and metrics with pm_live through apps/pm_app.hpp (D28); config/exec.cfg is the reference run file.
- [x] E4.2 Strict config parser (SEC4), hard bounds: src/pm/config.hpp; unknown, repeated, non-numeric, fractional-integer or out-of-bound values refuse the run (6/6 mutations caught). Venue mode now also runs R9 (loss on the worse of mid and best-bid marks), R13 (reject spike) and capital-days every risk tick (4/4 mutations caught); compat mode lifts R13 as the old model had none.
- [x] E4.3 Decision hash v2 (A4.2); same seed twice -> same hash; different seed with faults -> different hash. Engine::exec_hash covers intents with risk result, requests, reports, ledger after each report, kill; decision_hash_v2 mixes it with the quote hash. Reference recording, config/exec.cfg: seed 1 twice f6aa89e3b71dadac; with faults 2eca2f516dd4264e twice; seed 2 with faults 21bdd233151e321b.
- [x] E4.4 Paper mode on live feed with inline SimVenue (reuse pm_live feed/ring/recorder). `pm_exec --paper --run OUT`: a 90 s paper run replays (`pm_exec --replay OUT`) to the same three hashes.
- [x] E4.5 Metrics A5 through existing hand-off; kill via SIGUSR1 and KILL file (D11, D13); SEC3 check. Kill: both triggers tested on paper runs (all orders cancelled, risk rejects Killed after, kill.log, latch refuses the next start, --reset-kill clears it; replay of the killed run reproduces it). SEC3: port on 127.0.0.1 only (ss -ltnp). A5 not yet: unknown_orders, cancel_races, per-operation latency (E6.1 measures it), leg_* (E5).
- [x] E4.5b Known bug: `pm_arb --help` (any unknown argument) throws filesystem_error uncaught; make all apps reject unknown flags with usage and exit 2. apps/args.hpp; scripts/check_cli.sh (8 apps failed before, none after; a missing input directory also exits 2).
- [ ] E4.6 Ask: retire pm_live in favour of pm_exec, or keep both? Default: keep both until E7.
- G4: replay determinism; 1 h paper run: zero illegal transitions, zero reconcile mismatch; TSan 5 min paper run clean.

## E5 Arb executor

- [ ] E5.1 Unit tests on constructed books: X2 filters, X3 net edge, X4 sizing, X7 complete vs unwind choice, R10/R15 bounds.
- [ ] E5.2 src/exec/arb_exec.hpp, policies P and Q (X5), X9 one attempt per group.
- [ ] E5.3 Fault campaign: leg faults (V9, V10, V11, V14 on one leg) at high rate on reference recording: every incomplete set completed, unwound, or frozen within bounds; no silent residual.
- G5: all above.

## E6 Demo and benchmarks (D12)

- [ ] E6.1 Benchmarks on the reference recording (native, pinned): per-stage latency p50/p99/p99.9 (parse, engine, risk, OMS, sim), messages/s, intent-to-request p99; table in docs/exec/RESULTS.md with the command for each row.
- [ ] E6.2 Demo scripts in `scripts/demo/`, each one command, printing a known decision hash:
  (a) clean replay with maker + arb;
  (b) replay with core faults on: OMS reaches Unknown, reconciles, ledger mismatch zero;
  (c) leg-risk incident: one leg dropped, executor completes or unwinds within bound;
  (d) kill switch: trip by KILL file mid-run, all orders cancelled, no new orders, kill.log written.
- [ ] E6.3 pm_exec summary output: arb windows seen / filtered (by filter) / attempted / completed / incomplete (by action), maker fills, net PnL after fees, at measured RTT and 50/150/300 ms (D4). Record in RESULTS.md as measured, with the V-limits.
- G6: every number in RESULTS.md reproducible from its command line; every demo prints its expected hash.

## E7 Hardening

- [ ] E7.1 CI: alloc test, exec bench smoke, oms_fuzz 60 s.
- [ ] E7.2 Security gates SEC-G1..G5.
- [ ] E7.3 README/ROADMAP: measured numbers only; INDEX status -> done.
- G7: CI green, gates clean.

## Stop conditions (any phase)

- A venue fact breaks the design (E0) -> ask.
- Parity not exact (E3) -> ask.
- Determinism or invariant failure not understood the same day -> stop, report.
- E6.3 shows no actionable windows -> that is the measured output; record it, no threshold fishing.
