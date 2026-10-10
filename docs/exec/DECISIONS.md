# Decisions

Format: id, decision, why. Change a decision only by adding a new id that supersedes it.

## Fixed

- D1 Live order entry: never. No live venue, no order endpoint, no keys, no signing code. Why: owner answer 2026-10-10.
- D2 Scope: maker and arbitrage both route through intent -> risk -> OMS -> SimVenue. Maker's internal fill model moves to SimVenue; parity check (G3b) required. Why: owner answer; parity is a free oracle.
- D3 Paper account sized like a large professional desk (owner: "realistic to big brokers"):
  capital 1,000,000 USD; per-attempt loss bound 1,000 USD (0.1%); daily loss stop 20,000 USD (2%);
  per-market position cap 50,000 USD notional (5%); gross exposure cap 500,000 USD (50%);
  per-event-group cap 100,000 USD (10%). All config values, not constants. Why: owner answer, mapped to common desk ratios. Revisit if owner disagrees (Q1).
- D4 Latency settings in the report: measured RTT (E0.4) plus 50, 150, 300 ms one-way-equivalent. Why: owner answer.
- D5 Units for Qty and Usd: TBD at E0.3 from venue size precision. Candidates: Qty = 1e-6 share, Usd = micro-dollar (1e-6), Px = 1e-4. Must make price*qty exact in int64 with headroom: 10000 * qty_units * max_qty < 9.2e18.
- D6 No Venue interface/abstraction: SimVenue is the only venue (D1). Called directly. Why: one implementation, no indirection.
- D7 SimVenue runs inline on the trading thread, scheduled by `engine::EventQueue` (existing; kinds Market < MarketData < OrderArrival < Report at equal time). Why: deterministic, conservative tie-break, already tested.
- D8 Reuse map (confirm at E0.2): `engine/scheduler.hpp` as is; `backtest/risk.hpp` and `backtest/accounting.hpp` ideas, re-typed for Px/Qty/Usd (they are ITCH-typed: uint32 price, book::Side); `core/philox.hpp` for fault draws; `core/pool.hpp` + `book/id_map.hpp` LinearMap for order table; `core/histogram.hpp` for latency metrics; pm_live metrics hand-off (`Metrics::offer/fold`) for new metrics.
- D9 (superseded by D12)
- D10 Sell-side arb windows (bids sum > 1) are reported, not executed. Why: needs minting full sets on the conditional-token contract, unmodelled.
- D11 Kill switch reset is never automatic. Trip via: any risk rule, SIGUSR1, or file `<run_dir>/KILL` (D13: no HTTP trigger). Reset: restart with `--reset-kill` after reading the kill log. Why: tripping is the safe direction; reset needs a human.
- D12 Engineering project, not research. Success = correctness (oracles, invariants), determinism (hash), latency budgets met, faults handled, runnable demos. Arb/maker PnL is system output, reported as measured; thresholds are plain config, no tune/evaluate split. Why: owner 2026-10-10.
- D13 No new web surface: HTTP stays read-only metrics + existing dashboard, frozen. No `POST` endpoints. Why: focus (owner).
- D14 Faults: core four required (V9 drop ack, V10 drop fill, V11 duplicate, V14 disconnect); V8, V12, V13 optional, only if time allows. Why: the four cover every OMS recovery path.
- D15 ITCH side frozen: no further work on ITCH replay/book/backtest beyond keeping tests green. Why: focus on the execution layer.

## Open (owner)

- Q1 D3 mapping OK? (capital 1M, 0.1% per attempt, 2% daily). Default: proceed with D3.
- Q2 Deadline for the project? Decides whether E6 demos and E7 fit fully. Default: plan as written, cut optional items first.

## Open (resolve in E0, record answer here)

- Q3 Venue order types / TIF set (GTC, FOK, FAK, GTD?).
- Q4 Tick sizes per market, how a tick change is announced on the market channel.
- Q5 Minimum order size, size precision (-> D5).
- Q6 Maker/taker fees, how charged (per fill, on notional or shares).
- Q7 Order and cancel rate limits.
- Q8 Fill reporting: can a fill precede the ack; fill ids for dedup.
- Q9 Multi-outcome ("negative risk") events: position conversion rules; does a complete Yes set pay exactly 1.
- Q10 Resolution: how and when payout happens; market close signal on the feed.
- Q11 RTT laptop -> exchange API host (20 TCP connects, median and p90).
