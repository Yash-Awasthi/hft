# Phases

Each step: [ ] todo, [x] done (commit id). Gate Gn must pass before the next phase starts.
Step template: do -> check. "Ask" = stop and ask the owner before continuing.

## E0 Facts and inspection (no product code)

- [ ] E0.1 Create branch `exec-layer` from `phase1-hotpath`. Save `pm_stats ~/data/pm` to `~/data/ref/pm_stats.tsv`; confirm baseline hash (INDEX).
- [ ] E0.2 Read in full: engine/scheduler.hpp, backtest/risk.hpp, backtest/accounting.hpp, engine/matching.hpp, pm/maker.hpp, pm/engine.hpp, pm/arb.hpp, core/pool.hpp, book/id_map.hpp. Confirm or amend D8 (reuse map).
- [ ] E0.3 Answer Q3..Q10 from the exchange's current public API docs; write `docs/exec/VENUE.md` (fact, source URL, date read). Fix D5 units.
- [ ] E0.4 Measure RTT to the exchange API host: 20 TCP connects, median/p90 (Q11). Record in VENUE.md.
- [ ] E0.5 Check recorded data for: tick_size_change events, market close/resolution events, min order size hints. Record counts in VENUE.md.
- Ask if any fact contradicts DESIGN (e.g. fills not attributable to orders, no complete-set payout).
- G0: VENUE.md complete with dates; D5 and D8 final; DECISIONS updated.

## E1 Types, ledger, risk

- [ ] E1.1 `.gitignore` secret patterns (SEC7).
- [ ] E1.2 src/exec/types.hpp (T1..T7) + static_asserts.
- [ ] E1.3 Ledger tests first (L2 identity property, L4 settlement, L3 capital-days), then src/exec/ledger.hpp.
- [ ] E1.4 Risk tests first: each R1..R8 boundary (limit pass, limit+1 step reject); token bucket refill; R9..R15 triggers; R16 trip blocks and cancels; then src/exec/risk.hpp.
- [ ] E1.5 Bench: pre-trade check p99 (R17 <= 50 ns).
- G1: tests pass all presets; bench within budget; mutation check (plant: R5 ignores open orders; R16 does not block) both caught.

## E2 OMS

- [ ] E2.1 Table test: every (state, event) pair -> expected state or S13.
- [ ] E2.2 Reference model (naive, std::map) for property test.
- [ ] E2.3 src/exec/oms.hpp: Pool order table, cl_id (T6), S1..S16, timeouts, Status/reconcile hook.
- [ ] E2.4 Property test 100k cases: random interleavings incl. dup/reorder/timeout vs reference: cum fill monotone, <= qty; open qty, reservations, positions equal reference.
- [ ] E2.5 fuzz/oms_fuzz.cpp; 10 min clean.
- [ ] E2.6 alloc_test extended: steady-state submit/ack/fill/cancel zero allocations.
- [ ] E2.7 Bench S17.
- G2: all above; mutation check (plant: overfill accepted; dup fill counted twice; reject not releasing reservation) all caught.

## E3 SimVenue

- [ ] E3.1 Matching V1..V7 with unit tests on constructed books (taker walk, overlay clear, maker ahead, Fok/Fak/Gtc, fees, rule rejects).
- [ ] E3.2 Invariants V15..V17 as always-on checks in tests and debug builds.
- [ ] E3.3 Faults (D14): core V9, V10, V11, V14 with Philox, each forced on in a test; V8, V12, V13 optional.
- [ ] E3.4 Parity G3b prep: run maker on reference recording, old path, dump fills (ns, token, px, qty) to file.
- [ ] E3.5 Maker quotes become intents (A2.7); fills via SimVenue with latency 0, faults off; compare dump: must be identical.
- Ask if parity is not exact after one day of investigation (stop condition).
- G3: (a) invariants hold on full reference recording under each fault mode; (b) maker parity exact.

## E4 pm_exec app

- [ ] E4.1 apps/pm_exec.cpp: replay mode over recording + exec.cfg + seed; event loop A1.
- [ ] E4.2 Strict config parser (SEC4), hard bounds.
- [ ] E4.3 Decision hash v2 (A4.2); same seed twice -> same hash; different seed with faults -> different hash.
- [ ] E4.4 Paper mode on live feed with inline SimVenue (reuse pm_live feed/ring/recorder).
- [ ] E4.5 Metrics A5 through existing hand-off; kill via SIGUSR1 and KILL file (D11, D13); SEC3 check.
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
