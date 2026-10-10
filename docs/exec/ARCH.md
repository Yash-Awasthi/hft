# Architecture

## A1 Data flow (one trading thread, deterministic)

```
market record (live ring or recording) ----+
SimVenue report (scheduled in EventQueue) -+--> event loop (single ordered stream)
clock record (100 ms) ---------------------+         |
                                                     v
  books (pm::TokenBook) -> strategies (TokenMaker, ArbExec) -> OrderIntent
                                                     |
                               PreRisk.check(intent) | reject -> strategy.on_reject
                                                     v
                               Oms.submit -> VenueReq -> SimVenue.on_request (at t + latency)
                                                     |
  SimVenue -> VenueRpt (at t + latency) -> Oms.on_report -> Ledger.apply -> PostRisk -> KillSwitch
                                                     |
                               strategy.on_order_update / on_fill
```

- A1.1 Everything above runs on the trading thread. No other thread touches books, OMS, ledger, risk.
- A1.2 Other threads: feed (live only, existing), recorder (existing), HTTP (existing, read-only metrics). Kill triggers (SIGUSR1, KILL file) set an atomic flag the loop reads once per iteration (D11, D13).
- A1.3 Time: event-loop receive clock `Ns`; SimVenue times are `recv_ns + drawn latency`. No wall-clock reads inside components.

## A2 Modules and files

| Id | File | Role | Depends on |
|---|---|---|---|
| A2.1 | src/exec/types.hpp | Px, Qty, Usd, Ns, Side, Tif, OrderIntent, VenueReq, VenueRpt, enums | - |
| A2.2 | src/exec/ledger.hpp | positions, cash, fees, PnL identity, capital-days, set counts | types |
| A2.3 | src/exec/risk.hpp | PreRisk (R1..R8), PostRisk (R9..R14), KillSwitch | types, ledger |
| A2.4 | src/exec/oms.hpp | order table, state machine S*, timeouts, reconcile | types, core/pool, book/id_map |
| A2.5 | src/exec/sim_venue.hpp | matching model V*, fault model, invariants | types, pm/book, core/philox, engine/scheduler |
| A2.6 | src/exec/arb_exec.hpp | window -> legs, leg-risk handling X* | types, oms, pm/arb, pm/book |
| A2.7 | src/pm/maker.hpp (changed) | quotes become intents; fill model removed (moved to A2.5) | exec/types |
| A2.8 | apps/pm_exec.cpp | event loop, replay/paper modes, config, metrics, kill | all above, pm/reader, pm/session |
| A2.9 | tests/exec_*_test.cpp | per module, property tests, fault campaign | |
| A2.10 | fuzz/oms_fuzz.cpp | arbitrary report sequences into Oms | oms |
| A2.11 | bench: exec cases in hft_bench | latency budgets R-budget, S-budget | |

- A2.12 `pm_live` stays unchanged as the no-orders path until E4 decides whether `pm_exec` replaces it (Q in E4.6).

## A3 Interfaces (plain functions, no virtuals; D6)

- `PreRisk::check(const OrderIntent&, const Ledger&, const Oms&, Ns) -> Reject` (enum, None = pass).
- `Oms::submit(const OrderIntent&, Ns, Sink) -> uint64 cl_id | 0`; Sink receives VenueReq.
- `Oms::cancel(uint64 cl_id, Ns, Sink) -> bool`.
- `Oms::on_report(const VenueRpt&, Ns, Listener)`; Listener gets order updates and fills.
- `SimVenue::on_market(const pm::Event&, Ns)`; `SimVenue::on_request(const VenueReq&, Ns)`; `SimVenue::due(Ns, Sink)` emits reports whose time has come.
- `ArbExec::on_window(const ArbWindow&, books, Ns, IntentSink)`; `ArbExec::on_update(...)`.

## A4 Record and replay

- A4.1 Paper/replay with inline SimVenue: venue reports are a pure function of (market stream, config, seed). Record = existing market recording + `exec.cfg` + seed. No report records needed.
- A4.2 Decision hash v2 covers: every OrderIntent, risk reject, VenueReq, VenueRpt, ledger delta, kill event. Old hash (maker quotes only) stays computable for baseline comparison until E3 parity is proven.
- A4.3 Run dir layout: `<run>/session.tsv`, `<run>/c0/*.jsonl.zst` (existing), `<run>/exec.cfg`, `<run>/summary.tsv`, `<run>/kill.log`.

## A5 Metrics (via existing off-thread hand-off)

orders_total, rejects_total{reason}, fills_total, open_orders, unknown_orders, cancel_races_total, leg_incomplete_total{action}, leg_exposure_usd, kill_state, oms_op_ns{p50,p99}, risk_ns{p50,p99}, sim_latency_ns, pnl_usd, capital_locked_usd, capital_days.
