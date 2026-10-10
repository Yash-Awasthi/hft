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
- [ ] E3.3 Faults V8..V14 with Philox; each fault forced on in a test.
- [ ] E3.4 Parity G3b prep: run maker on reference recording, old path, dump fills (ns, token, px, qty) to file.
- [ ] E3.5 Maker quotes become intents (A2.7); fills via SimVenue with latency 0, faults off; compare dump: must be identical.
- Ask if parity is not exact after one day of investigation (stop condition).
- G3: (a) invariants hold on full reference recording under each fault mode; (b) maker parity exact.

## E4 pm_exec app

- [ ] E4.1 apps/pm_exec.cpp: replay mode over recording + exec.cfg + seed; event loop A1.
- [ ] E4.2 Strict config parser (SEC4), hard bounds.
- [ ] E4.3 Decision hash v2 (A4.2); same seed twice -> same hash; different seed with faults -> different hash.
- [ ] E4.4 Paper mode on live feed with inline SimVenue (reuse pm_live feed/ring/recorder).
- [ ] E4.5 Metrics A5 through existing hand-off; `POST /kill`, SIGUSR1, KILL file (D11); SEC3/SEC8 checks.
- [ ] E4.6 Ask: retire pm_live in favour of pm_exec, or keep both? Default: keep both until E7.
- G4: replay determinism; 1 h paper run: zero illegal transitions, zero reconcile mismatch; TSan 5 min paper run clean.

## E5 Arb executor

- [ ] E5.1 Unit tests on constructed books: X2 filters, X3 net edge, X4 sizing, X7 complete vs unwind choice, R10/R15 bounds.
- [ ] E5.2 src/exec/arb_exec.hpp, policies P and Q (X5), X9 one attempt per group.
- [ ] E5.3 Fault campaign: leg faults (V9, V10, V13, V14 on one leg) at high rate on reference recording: every incomplete set completed, unwound, or frozen within bounds; no silent residual.
- G5: all above.

## E6 Report

- [ ] E6.1 Split recordings by date: earlier half = tune, later half = evaluate (D9). List the split in the report.
- [ ] E6.2 Tune A_min, S_fresh, E_min, slip_ticks on the tune half only; freeze in `docs/exec/frozen.cfg` with commit id.
- [ ] E6.3 Evaluate once on the later half: per policy (P, Q) x latency (D4): windows seen, filtered by filter, attempted, completed, incomplete by action, net PnL after fees, capital-days, PnL per capital-day. Plus maker results through the OMS.
- [ ] E6.4 Write docs/exec/REPORT.md with the command line and seed that reproduce every number and the V-limits.
- Ask before re-running evaluation with any change (invalidates the result, D9).
- G6: report reproducible from its command lines.

## E7 Hardening

- [ ] E7.1 CI: alloc test, exec bench smoke, oms_fuzz 60 s.
- [ ] E7.2 Security gates SEC-G1..G5.
- [ ] E7.3 README/ROADMAP: measured numbers only; INDEX status -> done.
- G7: CI green, gates clean.

## Stop conditions (any phase)

- A venue fact breaks the design (E0) -> ask.
- Parity not exact (E3) -> ask.
- Determinism or invariant failure not understood the same day -> stop, report.
- E6 shows no actionable windows -> that is the result; report it.
