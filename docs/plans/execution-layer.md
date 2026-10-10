# Execution layer: plan

Status: planned, not started. Branch to work on: `exec-layer`, from `phase1-hotpath`.

The system can see the market (feed, books, replay) and decide (maker, arbitrage scanner),
but it cannot act: fills are simulated inside the strategy and there are no orders, acks,
rejects or cancels anywhere. This plan adds the layer between a decision and an exchange,
tests it against a simulated exchange that fails in all the ways a real one does, and runs
the arbitrage scanner's windows through it to get the first net-of-everything result.

## 1. Goals and non-goals

Goals:

1. An order manager (OMS) with an explicit per-order state machine, unique client order ids,
   and no allocation or syscall on the trading thread.
2. Pre-trade risk on every order and every cancel, post-trade risk on every fill and timer.
3. A kill switch that cancels everything, blocks new orders and needs a manual reset.
4. A simulated venue (`SimVenue`) that runs on recorded books with configurable latency,
   queue position, partial fills, rejects, dropped and reordered messages and disconnects,
   deterministic for a given seed.
5. An arbitrage executor that turns scanner windows into multi-leg orders and handles leg
   risk (one leg fills, another does not) by completing or unwinding within a loss bound.
6. Record and replay of the whole loop, market events plus venue responses, with a decision
   hash that covers orders, so any run reproduces bit for bit.
7. A report: windows seen, attempted, completed, unwound, net PnL after fees and slippage,
   capital locked and for how long.

Non-goals for this plan:

- Sending a real order. No code path in this plan opens a connection to an order endpoint.
  Live order entry is a separate plan and needs the owner's account, keys and decision.
- A new alpha signal. The maker and the scanner stay as they are; this plan is about acting
  on their decisions correctly.
- Portfolio optimisation or cross-venue hedging.

## 2. Ground rules (apply to every phase)

- Every change is measured or tested before it is committed; every phase ends with CI green
  on all presets, `pm_live --replay` decision hash unchanged for the parts that existed
  before, and a new hash recorded for the parts that are new.
- Nothing the trading thread runs may allocate, lock, log to a file, or make a syscall in
  steady state. The existing `alloc_test` pattern is extended to the new code.
- All money and size arithmetic is integer. Prices stay in 1e-4 units (the contract pays
  10000). Sizes and cash units are fixed in phase E0 after the venue's precision is
  verified (candidates: shares in 1e-2 or 1e-6 units, cash in micro-dollars). No `double`
  in the OMS, risk or ledger; `double` stays only in the maker's model.
- Illegal state transitions never pass silently: they increment a counter, log the order,
  and trip the kill switch.
- Every component is a pure function of its ordered inputs plus a seed. Wall clock is read in
  one place (the event loop) and passed in.
- Every failure path has a test that drives it on purpose (fault injection), not only the
  happy path.
- No secrets in the repository, ever. `.gitignore` covers `*.key`, `*.pem`, `.env`,
  `secrets/`. The plan adds no code that reads keys.

## 3. Architecture

```
 market events (live feed or recording)
        |
        v
 +------------------------- trading thread (single, deterministic) -------------------------+
 |  books (TokenBook) -> strategies (TokenMaker, ArbExecutor) -> OrderIntent                 |
 |                                                       |                                   |
 |                                                       v                                   |
 |                                              PreTradeRisk (checks, token buckets)         |
 |                                                       | pass          | reject -> strategy |
 |                                                       v                                   |
 |                                                OMS (order table, state machine)           |
 |                                                       | VenueRequest                      |
 |  Ledger (positions, cash, PnL identity) <- fills -----+---------------------------------+ |
 |  PostTradeRisk (loss, inventory, stale, mismatch) -> KillSwitch                         | |
 +-----------------------------------------------------------------------------------------+-+
                                                         |                          ^
                                          VenueRequest   v                          | VenueReport
                                 +----------------------------------------------------+
                                 | Venue interface                                    |
                                 |   SimVenue  (replay and paper; inline, scheduled)  |
                                 |   LiveVenue (later plan; own thread, SPSC rings)   |
                                 +----------------------------------------------------+
```

- In replay and paper mode the `SimVenue` runs inline on the trading thread, driven by the
  existing `engine::EventQueue` (its kinds already order market events before our order
  arrivals and reports at equal time, the conservative tie-break).
- The `Venue` boundary is a plain struct of two functions (submit, cancel) and a report
  callback, not a virtual interface; `LiveVenue` would sit behind two `SpscRing`s exactly
  like the feed does today.
- Recording: every `VenueRequest` and `VenueReport` is written into the same ordered record
  stream as market events, so `--replay` reproduces fills as well as decisions.

## 4. Data types (src/exec/types.hpp)

```
using Px  = std::int32_t;   // 1e-4, 0..10000
using Qty = std::int64_t;   // size units fixed in E0
using Usd = std::int64_t;   // micro-dollars
using Ns  = std::int64_t;   // receive-clock nanoseconds

enum class Side : uint8_t { Buy, Sell };
enum class Tif  : uint8_t { Gtc, Fok, Fak };      // confirm the venue's set in E0
struct OrderIntent { uint32_t token; Side side; Px px; Qty qty; Tif tif; uint32_t strategy; uint32_t tag; };
struct VenueRequest { enum Kind : uint8_t { New, Cancel } kind; uint64_t cl_id; uint32_t token; Side side; Px px; Qty qty; Tif tif; };
struct VenueReport  { enum Kind : uint8_t { Ack, Reject, Fill, CancelAck, CancelReject, Expired } kind;
                      uint64_t cl_id; uint64_t venue_id; Px px; Qty qty; Usd fee; uint16_t reason; Ns venue_ns; };
```

- `cl_id` = 16-bit session number, 48-bit sequence; never reused within a session, and the
  session number is persisted so a restart never collides with an earlier run.
- Order table: index-addressed `Pool<Order>`, `cl_id -> index` through the existing
  `LinearMap` (or a direct index, since the sequence is dense within a session).

## 5. OMS state machine (src/exec/oms.hpp)

States: `PendingNew, Live, PartiallyFilled, PendingCancel, Filled, Cancelled, Rejected,
Expired, Unknown`.

| From | Event | To | Notes |
|---|---|---|---|
| (none) | submit passes risk | PendingNew | open qty reserved in risk |
| PendingNew | Ack | Live | |
| PendingNew | Reject | Rejected | reservation released |
| PendingNew | Fill (before Ack) | PartiallyFilled / Filled | allowed: venues can fill before the ack arrives |
| PendingNew | timeout | Unknown | no new orders on that token until resolved |
| Live / PartiallyFilled | Fill | PartiallyFilled / Filled | overfill (cum > qty) is illegal |
| Live / PartiallyFilled | cancel sent | PendingCancel | |
| PendingCancel | CancelAck | Cancelled | |
| PendingCancel | Fill | PendingCancel / Filled | fills racing a cancel are normal |
| PendingCancel | CancelReject (too late) | Live / Filled | depends on remaining qty |
| PendingCancel | timeout | Unknown | |
| Unknown | any report | the state the report implies | then reconcile |
| terminal | anything but a duplicate | illegal | counter, log, kill switch |

Rules:

- Duplicate reports (same venue fill id) are idempotent: detected and dropped, counted.
- Out-of-order reports (cancel ack before an earlier fill) are applied by quantity, never by
  arrival order alone; cumulative filled qty only grows.
- `Unknown` freezes the token and triggers a reconcile query (in the sim: a full order
  status report); trading on the token resumes only when every order on it is known.
- Every transition is O(1), no allocation; the table and the transitions are `constexpr`
  data so the state machine is testable exhaustively.

## 6. Risk (src/exec/risk.hpp, reusing backtest/risk.hpp ideas)

Pre-trade, in this order (cheapest and most fundamental first), on every order:

1. Kill switch not tripped; token not frozen (`Unknown` orders, market closed, tick change
   pending).
2. Price on the token's current tick grid and inside 1..9999; size a multiple of the
   venue's size step and at least its minimum.
3. Price collar: not more than N ticks through the opposite best (fat-finger guard).
4. Self-cross: a buy at or above our own resting sell on the same token, or the reverse, is
   rejected.
5. Position including open orders: `|position + open buys - open sells (worst side)| <=
   limit` per token; the same per event group and in total gross notional.
6. Balance: cash reserved for open buys plus this order <= available cash (paper balance in
   the sim).
7. Order-rate and cancel-rate token buckets, per token and global, below the venue's
   published limits by a safety margin.
8. Max open orders per token and globally.

Post-trade and on the 100 ms risk tick:

- Loss stop on realised plus marked PnL (marking at the mid and, separately, at the
  liquidation side of the book; the stop uses the worse of the two).
- Gross inventory cap (pauses quoting, as today).
- Stale feed per connection (pauses its tokens).
- Reject-rate spike: more than K rejects in T seconds trips the kill switch.
- Ledger mismatch on reconcile: trips the kill switch.
- Leg-risk exposure above its bound for longer than T seconds: trips the kill switch.

Kill switch: one flag, set by any check or by the operator (HTTP `POST /kill` on
127.0.0.1, signal, or a file). On trip: cancel every open order, refuse every new one, log
the reason with the full state, and require an explicit reset command. Resetting is never
automatic.

Latency budget on the trading thread, measured by microbenchmark: pre-trade checks <= 50 ns,
an OMS transition <= 50 ns, a full intent-to-request path <= 200 ns at p99.

## 7. Ledger (src/exec/ledger.hpp)

- Per token: position, average cost, realised PnL, fees; per event group: set count (full
  sets of outcomes held, which pay exactly 1 at resolution).
- Integer accounting with the identity checked after every fill and every mark, as
  `backtest/accounting.hpp` does: total PnL = realised + unrealised + fees.
- Capital accounting: cash locked in open orders and in held positions, and capital-days
  (cash locked times time held), because a complete set pays only at resolution, which can
  be weeks away. Returns are reported per capital-day, not only in dollars.

## 8. Simulated venue (src/exec/sim_venue.hpp)

Inputs: the same market events the strategies see (L2 books and trades), our requests, a
latency model and a fault model, a Philox seed.

Matching model, per token:

- Order-entry latency: a request reaches the venue at `send + latency`, drawn from a
  configurable distribution (fixed, or empirical from a measured RTT sample). Reports come
  back after a second draw. Both default to a conservative WAN figure, not a colocated one.
- Taker part: on arrival, an order crossing the book walks the displayed levels as of the
  arrival time, filling at each level's price up to its displayed size. Liquidity we take is
  recorded in a per-level consumption overlay, so a second order does not take the same
  shares again; the overlay for a level clears when the exchange's next update for that level
  arrives (the update already reflects reality).
- Maker part: a resting order joins the back of its level: `ahead` = displayed size at
  arrival. Trades at the price reduce `ahead` first; a trade through the price fills it;
  a level shrinking below `ahead` pulls `ahead` down (cancels ahead of us are not credited:
  conservative). This is the model in `pm/maker.hpp`, moved here so it has one home.
- FOK fills completely or not at all; FAK fills what is there and cancels the rest; GTC
  rests.
- Fees: per the venue's fee schedule (maker and taker rates verified in E0), charged on each
  fill.
- Rule rejects: off-tick price, below minimum size, market closed or resolved, insufficient
  paper balance, price outside bounds.

Fault model (each a probability or a schedule, all from the Philox stream so a seed
reproduces them):

- reject an order that would otherwise pass; drop an ack; drop a fill report (the fill still
  happened: the ledger must catch it at reconcile); duplicate a report; deliver a cancel ack
  before a fill that happened earlier; delay a report beyond the OMS timeout; disconnect for
  T seconds (all reports during it arrive afterwards, in order, or are lost and found only
  at reconcile).

Invariants checked by the sim itself after every event (abort on violation in tests):
fill qty <= order qty; a buy never fills above its limit nor a sell below; filled shares
taken from a level never exceed its displayed size plus our overlay; venue-side positions
equal the sum of fills.

Honest limits, written into every report the sim produces: L2 data has no order ids, so
queue position is estimated; adverse selection is only partly captured (fills against a
book that is about to move are counted as the data shows, but our own presence does not
move anyone else); latency is a model.

## 9. Arbitrage executor (src/exec/arb_exec.hpp)

Input: a scanner window (group, side, edge, size on the thinnest leg, legs with their
prices and displayed sizes) and the current books.

Decision to act:

- Net edge = gross edge - taker fees on every leg - a slippage allowance per leg (ticks)
  - a latency allowance (expected price move over the round trip, estimated from the
  recording). Act only if net edge >= threshold and the window has survived a minimum age
  (filters the sub-millisecond split-message artefacts already seen in the data) and every
  leg's book is fresh (no leg older than S ms since its last update).
- Size = min(thinnest leg displayed size, per-group risk cap, available cash / cost of one
  set), rounded down to the size step; skip if below the venue minimum.

Placing the legs (both policies implemented, chosen by config, compared in the report):

- Parallel: every leg sent at once as FAK at its window price. Smallest time skew, largest
  chance of an incomplete set.
- Sequential: thinnest or least liquid leg first as FOK; on fill, the rest as FAK. Fewer
  incomplete sets, more time skew.

Leg risk, when a set is incomplete after all reports are in (or after a timeout):

1. Compute the residual: which legs are filled and by how much.
2. Cost to complete: buy the missing legs at the current asks (walking depth).
3. Cost to unwind: sell the filled legs at the current bids (walking depth).
4. Do the cheaper one if its loss is within the per-attempt loss bound; otherwise hold the
   residual, freeze the group, and alert. Holding is allowed only within the leg-risk
   exposure bound; beyond it the kill switch trips.
5. Every incomplete set is logged with its cause (which leg, which fault), the action taken
   and its cost.

Sell-side windows (bids summing above 1) need full sets to sell, which means minting sets
from collateral on the venue's conditional-token contract. Until that operation is modelled
and verified, sell-side windows are reported but not executed.

## 10. Determinism, record and replay

- The trading thread's input is one ordered stream: market records, venue reports, clock
  records. In paper mode with the inline sim, venue reports are a function of that stream
  and the seed, so only the seed and the config need recording; in live mode (later) the
  reports themselves are recorded.
- The decision hash extends to cover every `VenueRequest` and every ledger change.
- `pm_exec --replay DIR --seed S` must give the same hash twice, and a different seed must
  give a different hash whenever any fault fired.

## 11. Tests and checks

| Check | What it proves |
|---|---|
| State-machine table test | every (state, event) pair: the expected next state or an illegal-transition trip |
| OMS property test (RapidCheck) | random interleavings of acks, fills, duplicates, reorders, cancels and timeouts against a naive reference model: cumulative fill never decreases, never exceeds qty; open qty, position and reservations always equal the reference |
| Risk tests | each check rejects exactly at its boundary (limit, limit + 1 step); token bucket refills at the configured rate; kill switch blocks and cancels, reset is explicit |
| Ledger property test | identity holds after every event; a complete set is worth exactly 1 at resolution; capital-days add up |
| Sim invariants | the section 8 invariants on a full recorded day under every fault mode |
| Fault campaign | one run per fault type at a high rate on a recorded day: the system ends with zero ledger mismatch after reconcile, or a kill-switch trip naming the fault, never silently wrong |
| Replay determinism | same seed twice: same hash; replay of a paper run: same hash |
| No allocation | `alloc_test` drives intent -> risk -> OMS -> sim -> ledger in steady state: zero heap allocations |
| Latency microbench | section 6 budgets, as Google Benchmark cases, run in CI as a smoke test |
| ThreadSanitizer | live paper run with the HTTP thread and the kill endpoint under TSan |
| Fuzz | an OMS fuzz target: arbitrary report sequences never crash, never break the invariants |
| Mutation check | plant at least four bugs (overfill accepted, duplicate fill counted twice, reservation not released on reject, kill switch not blocking) and confirm each is caught |

## 12. Phases

Each phase ends with its exit gate met and a commit; no phase starts before the previous
gate is met.

### E0. Inspection and facts (no code)

- Read `engine/scheduler.hpp`, `backtest/risk.hpp`, `backtest/accounting.hpp`,
  `engine/matching.hpp`, `pm/maker.hpp`, `pm/engine.hpp` in full; decide per piece: reuse as
  is, generalise (template on price and size types), or copy the idea.
- Verify from the exchange's current public API documentation and record in
  `docs/venue-facts.md` with links and the date read: order types and time-in-force values;
  tick sizes and how a tick change is announced; minimum order size and size precision;
  maker and taker fees; order and cancel rate limits; what an order ack and a fill report
  contain; how fills are reported (user channel) and whether a fill can precede an ack;
  cancel-all; any dead-man or heartbeat cancel; how multi-outcome ("negative risk") events
  convert positions; how and when a market resolves and pays.
- Measure the round-trip time from this laptop to the exchange's API host (TCP connect time,
  20 samples) for the latency model's default.
- Exit gate: venue facts written, units for `Qty` and `Usd` chosen, reuse decisions written
  at the top of this file.

### E1. Types, ledger, risk

- `src/exec/types.hpp`, `ledger.hpp`, `risk.hpp` with tests from section 11.
- Exit gate: ledger and risk tests and properties pass; microbench within budget.

### E2. OMS

- `src/exec/oms.hpp`: order table, state machine, timeouts, reconcile hook, kill switch.
- Exit gate: table test, OMS property test (100k cases), fuzz 10 minutes clean, mutation
  check catches all planted bugs, zero allocations.

### E3. Simulated venue

- `src/exec/sim_venue.hpp`: latency, taker walk with overlay, maker queue, FOK/FAK/GTC,
  fees, rule rejects, fault model, invariants.
- Move the maker's fill model out of `pm/maker.hpp` into the venue; the maker then quotes
  through the OMS. Before switching, record the maker's fills on the reference recording
  through both paths: with faults off and zero latency they must match exactly.
- Exit gate: invariants hold on the full reference recording under every fault mode; the
  maker parity check passes.

### E4. Wiring and replay

- `apps/pm_exec.cpp`: replay and paper modes, config file, seed, decision hash over
  orders; HTTP metrics extended (orders, rejects by reason, fills, open orders, leg-risk
  exposure, kill-switch state) using the existing off-thread metrics hand-off; `POST /kill`.
- Exit gate: replay determinism checks pass; paper run on the live feed for 1 hour with no
  illegal transition, no ledger mismatch, TSan clean on a shorter run.

### E5. Arbitrage executor

- `src/exec/arb_exec.hpp`: section 9, both placement policies, leg-risk handling.
- Exit gate: unit tests for complete-versus-unwind choices on constructed books; fault
  campaign with leg faults injected ends with every incomplete set resolved or frozen within
  the bounds.

### E6. Report

- Run the executor over every recording available, split by date: tune the thresholds on
  the earlier half only, report on the later half only (no tuning on the evaluation data).
- Report: windows seen, filtered (and by which filter), attempted, completed sets,
  incomplete (completed later / unwound / frozen), net PnL after fees and slippage,
  capital locked and capital-days, per policy and per latency setting (for example 50 ms,
  150 ms, 300 ms), with the sim's honest limits stated.
- Exit gate: the report is reproducible from a command line and a seed.

### E7. Hardening

- Alloc test, latency microbench and the OMS fuzz target join CI; README and ROADMAP updated
  with measured numbers only.

## 13. Cautions

- Simulated PnL is an upper bound with an unknown gap: queue position is estimated from L2,
  other traders do not react to our orders, and latency is modelled. A positive result in
  E6 is a reason to measure more, not to trade.
- Small numbers: early data shows event windows with a thinnest leg of 8 to 70 shares.
  After fees and the venue minimum size, many windows may not be actionable at all; the
  report must say so rather than tune thresholds until something appears.
- Tuning on the evaluation half, or re-running E6 with new thresholds after seeing its
  result, invalidates the result. Thresholds are frozen before the evaluation run.
- Capital lock: a complete set pays at resolution, possibly weeks later. A small edge over a
  long lock can be a poor return; report per capital-day.
- Tick-size changes mid-session: on a change, cancel and freeze the token until requoted on
  the new grid.
- Market close and resolution: stop trading the market, cancel its orders, settle the
  ledger at the resolved value.
- Reconnects: on a feed reconnect, books are invalid until a snapshot (already the case);
  orders on those tokens are cancelled, not left resting against a book we cannot see.
- Clock: receive time comes from the local clock; exchange timestamps are in milliseconds
  and can disagree with ours. Neither is used to order events across connections.
- Fees and limits change. `docs/venue-facts.md` carries the date it was verified; re-verify
  before any live plan.
- Legal and account: whether the owner may trade on this exchange from their location, and
  with what account, is the owner's question and a precondition of any live plan.
- Keys: when a live plan exists, signing keys live outside the repository and outside the
  trading process's working directory, loaded once at start, never logged, never sent to
  the metrics endpoint. Signing uses an audited library (libsecp256k1 or equivalent), not
  hand-written cryptography.
- No code in this plan may contain an order endpoint URL. A grep for the exchange's order
  host in `src/` and `apps/` is part of the E7 gate.

## 14. Stop conditions

Stop and report instead of continuing if:

- a fact in E0 contradicts the design (for example, fills cannot be attributed to orders);
- the maker parity check in E3 does not reach exact agreement;
- any invariant or determinism check fails and the cause is not understood the same day;
- the E6 report shows no actionable windows: that is the result, and it is reported as such.

## 15. Questions for the owner (not blocking E0 to E7)

- Paper balance for the sim (default 1,000 USD) and the per-attempt loss bound (default
  5 USD).
- Latency settings to report (default 50, 150, 300 ms).
- Whether a live plan is wanted at all after E6, and from which machine and location.
