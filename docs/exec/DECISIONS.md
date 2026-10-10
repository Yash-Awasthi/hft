# Decisions

Format: id, decision, why. Change a decision only by adding a new id that supersedes it.

## Fixed

- D1 Live order entry: never. No live venue, no order endpoint, no keys, no signing code. Why: owner answer 2026-10-10.
- D2 Scope: maker and arbitrage both route through intent -> risk -> OMS -> SimVenue. Maker's internal fill model moves to SimVenue; parity check (G3b) required. Why: owner answer; parity is a free oracle.
- D3 Paper account sized like a large professional desk (owner: "realistic to big brokers"):
  capital 1,000,000 USD; per-attempt loss bound 1,000 USD (0.1%); daily loss stop 20,000 USD (2%);
  per-market position cap 50,000 USD notional (5%); gross exposure cap 500,000 USD (50%);
  per-event-group cap 100,000 USD (10%). All config values, not constants. Why: owner answer, mapped to common desk ratios. Revisit if owner disagrees (Q1).
- D4 Latency settings in the report: measured RTT 182 ms (VENUE F28: request round trip, median) plus 50, 150, 300 ms. Model: order-entry one-way = RTT/2 each direction; sports markets add the 1 s matching delay (F10). Why: owner answer.
- D5 Units (final): Px = 1e-4 USD (int32, 0..10000; covers ticks 0.0001..0.1 incl. 0.0025/0.005, F7). Qty = 1e-6 share (int64; book sizes carry up to 6 dp, orders 2 dp, F5/F6). Usd = 1e-10 USD (int64), so Px*Qty is exact with no rounding: max representable 9.2e8 USD, capital 1e6 USD uses 1e16. Exchange-side amounts (6 dp USDC) and fees (5 dp, F17) are produced by the F6/F17 rounding rules and are exact in Usd units. Accumulators that can exceed 9.2e8 USD (lifetime traded notional) use __int128 or are kept in shares. Why: one exact unit, no rounding inside the ledger.
- D6 No Venue interface/abstraction: SimVenue is the only venue (D1). Called directly. Why: one implementation, no indirection.
- D7 SimVenue runs inline on the trading thread, scheduled by `engine::EventQueue` (existing; kinds Market < MarketData < OrderArrival < Report at equal time). Why: deterministic, conservative tie-break, already tested.
- D8 Reuse map (final, E0.2 read in full): `engine/scheduler.hpp` as is (uint64 time works for epoch ns; reserve capacity up front, growth allocates); `backtest/risk.hpp` and `backtest/accounting.hpp`: ideas only, not code (single symbol; risk uses double mid/tokens and uint32 price; accounting keeps a std::deque that allocates; identity pattern and token bucket copied); `core/philox.hpp` for fault draws; `core/pool.hpp` + `book/id_map.hpp` LinearMap for order table; `core/histogram.hpp` for latency metrics; pm_live metrics hand-off (`Metrics::offer/fold`) for new metrics.
- D9 (superseded by D12)
- D10 Event-level (neg-risk, more than 2 markets) sell-side windows are reported, not executed. Pair-level: see D17. Why: event-level selling needs split in every market plus conversion; unmodelled.
- D11 Kill switch reset is never automatic. Trip via: any risk rule, SIGUSR1, or file `<run_dir>/KILL` (D13: no HTTP trigger). Reset: restart with `--reset-kill` after reading the kill log. Why: tripping is the safe direction; reset needs a human.
- D12 Engineering project, not research. Success = correctness (oracles, invariants), determinism (hash), latency budgets met, faults handled, runnable demos. Arb/maker PnL is system output, reported as measured; thresholds are plain config, no tune/evaluate split. Why: owner 2026-10-10.
- D13 No new web surface: HTTP stays read-only metrics + existing dashboard, frozen. No `POST` endpoints. Why: focus (owner).
- D14 Faults: core four required (V9 drop ack, V10 drop fill, V11 duplicate, V14 disconnect); V8, V12, V13 optional, only if time allows. Why: the four cover every OMS recovery path.
- D15 ITCH side frozen: no further work on ITCH replay/book/backtest beyond keeping tests green. Why: focus on the execution layer.
- D16 Settlement modelled: a fill is Matched, then Confirmed or Failed (F15). Ledger keeps matched and confirmed positions separately; risk uses matched (worst case); a Failed trade reverses its fill. Settlement failure is a core fault (V18), making five core faults with D14. Why: real venue behaviour; changes OMS and ledger.
- D17 Yes/No pair windows: buy side (Yes ask + No ask < 1) executes and then merges 1 Yes + 1 No -> 1 USD at once (F23), so capital is not locked to resolution; sell side (Yes bid + No bid > 1) executes by split 1 USD -> Yes + No, then sells both. Merge/split are ledger operations with a configurable latency (relayer) and zero fee (U, F23). Why: F23 makes pair windows complete trades.
- D18 Fees: taker only, fee = C * rate * p * (1 - p), rate by market category (F17/F18), rounded to 5 dp; makers pay nothing; rebates not modelled. Session file gains each market's category (from Gamma tags). Formula variant with exponent (F19) is a config switch, off by default.
- D19 Batch limit 15 orders (F11): event-level arb with n legs sends ceil(n/15) batches back to back; policy P skew measured.
- D20 Exchange timestamps are never used for latency or ordering (F29); only local receive time.
- D21 Paper fills are decided by trade price against our quote, never by the feed's trade side: public-feed trade direction matches on-chain truth in only ~59% of cases (RF27). A trade at p can fill our bid only if p <= bid and our ask only if p >= ask; bid < ask, so price alone decides. E3 measures feed side vs price-implied side agreement on our recordings. Why: RF27.
- D22 Timeline: about 5 months from 2026-10-10 (owner, no hard deadline) -> target 2027-03-10. Optional items (V8, V12, V13) only after G5.
- D24 Supersedes D18's rate source and D4's sports rule: each market's fee and delay come from the metadata API per market (Gamma market fields, checked 2026-10-10 on 40 events): `feesEnabled`, `feeSchedule {rate, exponent, takerOnly, rebateRate}`, `secondsDelay` (1 for sports games, else 0/null), `orderMinSize` (5), `orderPriceMinTickSize`. Rates differ inside a category (sports v2 0.03, v3 0.05), so no category table. Fee = qty * rate * (p (1 - p))^exponent (exponent 1 on every market seen = docs formula). Kept per token as `pm::MarketRules` in the session file. Recordings without rules: defaults (fees on, 0.05, exp 1, min 5, tick from book, delay 0).
- D25 Maker parity (G3b) runs SimVenue in a compatibility mode that picks the order hit by the feed's trade side, as pm/maker.hpp does today, with zero latency, no faults and instant replace; it must match the old fills exactly. Production mode uses D21 (price vs our quote); the difference between the two modes on the reference recording is reported as a number, not hidden. Why: D21 conflicts with exact parity on 1-7% of trades (F32).
- D26 Venue mode (E4): makers must take inventory from the ledger, not their own count: the fault campaign showed settlement failures change the ledger but not the maker's view (decision hash unchanged under 20% failures). Done: inventory is the ledger position and a failed settlement returns its cash to the maker; fault campaign settle-fail hash c4bc2ce32e95ef27 (same as no fault) -> 1a4108d4a39203b5.
- D27 Production maker asks need shares: the old maker quotes asks with zero or negative inventory (short), which the venue forbids (L7). Parity runs allow it (RiskLimits.allow_short, Ledger long_only=false, D25 only). Production (E4/E5): asks only up to held shares, or split first (D17). Done for the maker: ask size = min(size, available position) in 2-dp shares, skipped below the market minimum and retried at the next sync; split-first is left to the arb executor (E5). Reference recording, --venue, 50 ms + 20 ms jitter, seed 7: venue rejects 76 -> 151 (all post-only crosses on asks pinned to best bid + tick), maker fills 73 -> 96.
- D28 pm_exec and pm_live share one implementation (apps/pm_app.hpp); pm_exec adds the run file, venue mode always on, run directory outputs and kill triggers. An operator kill enters the trading loop as a `_kill` control record, so the recording carries it and a replay reproduces it. A paper run needs a new run directory and writes exec.cfg (as given) and overrides.cfg (--set lines, session number); `pm_exec --replay OUT` reads both. A kill leaves `~/data/exec/KILLED` (D11 latch, next to the T6 session counter). Why: one copy of the live machinery; replay determinism includes kills.
- D29 Arbitrage executor (E5): a group is a market pair when one of its two tokens is in no other group (the No), else an event; risk groups (D3 group cap) are events, formed by joining every group's tokens. The executor counts its own holdings from its order and settlement reports; the maker's inventory and ask size exclude them, so the maker never sells a set. One attempt per window; no attempts after a kill. Depth for sizing and completion excludes what we already took at a level (V24). R15 trips only when exposure (attempt cash not covered by complete sets) stays above attempt_bound for hold_limit; a smaller frozen residual is held, counted and retried once a second. Faults in the campaign act on all reports, not a chosen leg. Why: owner rule X9/R15 as written; no split per strategy in the ledger needed.
- D23 Recorders run continuously from pinned binaries in ~/data/bin (PM_RECORD_BIN, PM_LIVE_BIN); autostart at logon stays off (owner). Caps 50 GB + 20 GB kept (owner). Replace a pinned binary with `install` (new inode), never `cp` over it.

## Open (owner)

- Q1 resolved 2026-10-10: D3 accepted.
- Q2 resolved 2026-10-10: D22.

## Resolved in E0 (2026-10-10, details in VENUE.md)

- Q3 GTC, GTD, FOK, FAK; post-only on GTC/GTD (F1-F3).
- Q4 Ticks 0.1/0.01/0.001/0.0001; `tick_size_change` event on the market channel (F7, F26).
- Q5 Per-market `min_order_size` (shares assumed), size 2 dp, amounts 6 dp (F5-F8).
- Q6 Taker-only p(1-p) fee by category, 5 dp (F17-F19).
- Q7 Per-signer windows, POST /order 5000/10 s (F21).
- Q8 Trade ids exist; fill-before-ack not stated, kept possible (F16).
- Q9 Neg-risk NO -> YES in every other market; pair backed by 1 pUSD; merge/split (F22-F24).
- Q10 UMA oracle, 2 h challenge; `market_resolved` event carries winner (F25, F26).
- Q11 RTT 182 ms median request, 67 ms TCP to edge (F27, F28).

## Open (owner)

- Q4b resolved 2026-10-10: both (RF4, RF27).
