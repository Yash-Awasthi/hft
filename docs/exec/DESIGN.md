# Design

## T Types (src/exec/types.hpp)

- T1 `Px` int32 1e-4; `Qty` int64 (unit D5); `Usd` int64 micro-dollars (D5); `Ns` int64.
- T2 `Side {Buy, Sell}`; `Tif {Gtc, Fok, Fak}` (confirm Q3).
- T3 `OrderIntent {token u32, side, px, qty, tif, strategy u16, tag u32}`.
- T4 `VenueReq {kind New|Cancel, cl_id u64, token, side, px, qty, tif}`.
- T5 `VenueRpt {kind Ack|Reject|Fill|CancelAck|CancelReject|Expired|Status|Settled|SettleFailed, cl_id, venue_id u64, fill_id u64, px, qty, fee Usd, reason u16, venue_ns}`. Ack carries the venue status (live, matched, delayed, unmatched; F9, F10).
- T6 `cl_id = session(16) << 48 | seq(48)`; session persisted in `~/data/exec/session.counter`, incremented per run; never reused.
- T7 All structs trivially copyable, fixed size; static_assert sizes.

## S OMS state machine (src/exec/oms.hpp)

States: PendingNew, Live, Partial, PendingCancel, Filled, Cancelled, Rejected, Expired, Unknown.

| Id | From | Event | To | Effect |
|---|---|---|---|---|
| S1 | - | submit (risk pass) | PendingNew | reserve open qty/cash; emit New |
| S2 | PendingNew | Ack | Live | |
| S3 | PendingNew | Reject | Rejected | release reservation |
| S4 | PendingNew | Fill | Partial/Filled | fill before ack is legal |
| S5 | PendingNew | timeout T_ack | Unknown | freeze token; request Status |
| S6 | Live/Partial | Fill | Partial/Filled | cum += qty; cum > qty -> S13 |
| S7 | Live/Partial | cancel | PendingCancel | emit Cancel |
| S8 | PendingCancel | CancelAck | Cancelled | release remaining |
| S9 | PendingCancel | Fill | PendingCancel/Filled | race is legal |
| S10 | PendingCancel | CancelReject | Live/Partial/Filled | by remaining qty |
| S11 | PendingCancel | timeout T_cxl | Unknown | freeze token; request Status |
| S12 | Unknown | Status/any report | implied state | unfreeze when token has no Unknown |
| S13 | any | overfill, report for terminal order (not dup), unknown cl_id | unchanged | illegal++, log, kill |
| S14 | any | duplicate fill_id | unchanged | dup++, ignore |

- S18 Settlement (D16): each fill is Matched on arrival; Settled -> confirmed; SettleFailed -> the ledger reverses the fill (opposite fill, fee refunded). The order's cum and state are not changed: the venue considers the order filled. Counted (settle_failures).
- S15 Fill application is by quantity, idempotent by fill_id. Every fill id of an order is kept (list in a shared index-addressed pool, freed with the order). An 8-entry ring was tried first; the property test showed a duplicate of an older fill slipping through (order filled in > 8 pieces).
- S16 Transitions are one switch in Oms::on_report; the expected table (9 states x 10 events, incl. three Status replies) lives in tests/oms_test.cpp and is checked exhaustively.
- S19 Reconcile: VenueReq::Status asks the venue to resend every fill of the order (duplicates dropped by S15), then a Status report with its state (Live, Cancelled, NotFound) and cumulative fill; a cumulative fill that differs from ours trips R14 (Mismatch).
- S20 Finished orders stay keep_done (60 s) so late duplicates are recognised; then freed and the slot reused.
- S21 Reservations: buy reserves notional at the limit plus the worst-case taker fee in the ledger; each fill releases its share (notional at the limit, fee in proportion); the rest is released when the order finishes. Sell reserves the shares. Risk's Exposure (open buy notional, own best prices, counts) is updated on open, each fill and close.
- S17 Budget: transition <= 50 ns. Measured: full cycle (submit incl. risk, ack, fill, cancel, cancel ack) 57 ns = ~11 ns per operation (native, batch mean).

## R Risk (src/exec/risk.hpp)

Pre-trade, order (cheapest first; first failure returns its reason):

- R1 kill switch off; token not frozen (Unknown order, closed market, tick change pending, feed stale).
- R2 px on token tick grid, 1 <= px <= 9999; qty multiple of size step, >= venue min (Q5).
- R3 collar: buy px <= best_ask + C ticks; sell px >= best_bid - C ticks; requires two-sided fresh book.
- R4 self-cross: no buy >= own resting sell on same token, no sell <= own resting buy.
- R5 position incl. open orders, worst side: per token qty cap; per market notional cap (D3); per group cap (D3); gross cap (D3).
- R6 cash: reserved buys + this order notional + fees <= available cash.
- R7 token buckets: order rate and cancel rate, global, at <= 80% of F21 (POST /order 5000/10 s burst, 48000/10 min sustained: two buckets each).
- R8 open orders per token <= N_tok, global <= N_all.
- Cancels: R1 (kill does not block cancels), R7 cancel bucket only.

Post-trade / 100 ms tick:

- R9 daily loss: realised + marked PnL <= -daily_stop -> kill. Mark = min(mid mark, liquidation mark at book side with depth).
- R10 per-attempt loss (arb): attempt PnL <= -attempt_bound -> stop attempt, freeze group.
- R11 gross inventory cap -> pause new opening orders (closing allowed).
- R12 stale feed per connection -> cancel that connection's tokens' orders, freeze.
- R13 reject spike: > K rejects in T s -> kill.
- R14 reconcile mismatch (OMS vs SimVenue status, ledger vs sum of fills) -> kill.
- R15 leg exposure (incomplete arb set) > bound for > T_leg -> kill.

- R16 KillSwitch: trip(reason) -> cancel all open, block R1, write kill.log with full state; reset only by D11.
- R17 Budget: full pre-trade <= 50 ns p99.

## L Ledger (src/exec/ledger.hpp)

- L1 Per token: matched pos, confirmed pos (Qty), cost Usd, realised Usd, fees Usd. Per group: complete sets held (min over legs of pos). Cash: available, reserved (open buys), pending settlement.
- L6 Merge/split (D17): merge n Yes + n No -> n * 10000 Px-units of cash; split the reverse; both after a configurable latency, zero fee.
- L2 Identity after every fill and mark: total = realised + unrealised + fees (exact int).
- L3 Capital: locked_in_orders, locked_in_positions; capital_days += locked * dt each tick.
- L4 Resolution: settle each token at 0 or 10000; sets settle at 10000 per set.
- L7 Long only: the venue sells only shares held; risk rejects a sell above available position (pos - reserved). A negative position can only come from a failed settlement of shares already sold: counted as a deficit -> R14 kill.
- L8 Identity is cash - capital + cost == realised - fees (exact). It holds for any cost-removal rule, so cost basis correctness is covered by worked examples, not by the identity.
- L5 Overflow guard: static_assert / debug check on max notional (D5 headroom).

## V SimVenue (src/exec/sim_venue.hpp)

Matching:

- V1 Request arrives at t_send + L_in; report leaves venue at t_event and arrives at + L_out. L from config: fixed or empirical sample (D4); Philox draw per message (purpose ids distinct).
- V2 Taker: on arrival, walk displayed levels at arrival time; fill at each level price up to displayed - overlay[level]. overlay[level] += taken; cleared when a market update for that level arrives.
- V3 Maker: rest at px; ahead = displayed size at arrival (0 if px improves best). Trade at px reduces ahead, then fills; trade through px fills all remaining; level shrink below ahead -> ahead = level size. Cancels ahead not credited. (Same as pm/maker.hpp today; G3b parity.)
- V4 Tif: Fok all-or-none at arrival; Fak fill available, cancel rest (Expired report); Gtc rests.
- V5 Fees (D18): taker fills only, fee = qty * rate * p * (1 - p), rounded to 1e-5 USD; rate by market category; maker fills zero.
- V6 Rule rejects: off-tick, size not 2 dp or below min_order_size (F6, F8), closed/resolved market, cancel-only/closed-only mode (F12), paper cash insufficient, px out of bounds; amounts rounded per F6.
- V6b Matching delay: marketable orders in sports markets wait 1 s (F10) before matching; unmatched remainder rests (GTC/GTD) or is cancelled (FAK/FOK).
- V7 Market events: tick change -> reject resting orders off new grid (or cancel all on token, per Q4); close/resolve -> cancel all, settle.

Faults (each config prob or schedule; Philox, seed in exec.cfg):

- Core (D14, D16): V18 settlement failure (a matched fill later fails, configurable probability and delay); V9 drop ack; V10 drop fill report (fill happened; found at reconcile); V11 duplicate report; V14 disconnect T s (reports after reconnect in order, or lost -> reconcile).
- Optional: V8 spurious reject; V12 reorder (cancel ack before earlier fill); V13 delay > OMS timeout.

Invariants (checked every event; abort in tests):

- V15 fill qty <= order qty; buy fill px <= limit; sell fill px >= limit.
- V16 taken from level <= displayed + overlay accounting.
- V17 venue positions == sum of fills; venue cash == ledger cash at reconcile.

Limits (print in every report): L2 has no order ids (queue estimated); others do not react to us; latency modelled.

## V-impl SimVenue mechanics (E3)

- V20 Books: the venue reads the same per-token books the strategies see, through a callable book(t) (as ArbScanner does); no second copy.
- V21 Scheduling: one engine::EventQueue. Request arrival at t_send + L_in is OrderArrival; report delivery at t_venue + L_out is Report. At equal times Market < OrderArrival < Report (D7), so an order arriving in the same nanosecond as a market event sees the book after it.
- V22 Resting orders per token in a small index-addressed list: {cl_id, venue_id, side, px, rem, ahead, cum, fills}. Venue keeps its own record of every fill per order (for Status resends, S19).
- V23 New: rule checks (V6) -> Reject; post-only that would match -> Reject; marketable part with a token delay (rules.delay_ms) -> Ack(Delayed), match at +delay against the book then; else match at once: walk opposite levels best first while price within limit; per level take min(remaining, displayed - overlay); fill at the level price; FOK checks the total first (all or Expired); FAK fills then Expired for the rest; GTC/GTD rests the remainder with ahead = displayed size at its price (0 if it improves the best). Ack status: Live, Matched or Delayed.
- V24 Overlay: per token, per (side, price) quantity we took; cleared for a level when a price_change for that level arrives, for the token on a snapshot.
- V25 Maker fills on a trade at p, size s (production, D21): our resting buys at px >= p and sells at px <= p are candidates; px strictly better than p (trade through) fills all remaining; px == p consumes ahead first, then fills. Compat mode (D25): only the side the feed names (taker buy -> our sells) is candidate, as pm/maker.hpp. Maker fill price = our limit. Fee 0 (makers pay nothing, F17).
- V26 Level update at (side, px): ahead = min(ahead, new displayed size) for our orders there (cancels ahead not credited: V3).
- V27 Cancel arrival: resting -> CancelAck; already filled or unknown -> CancelReject. Status arrival: resend every recorded fill of the order, then Status(state, cum); order never seen (dropped New) -> Status NotFound.
- V28 Settlement: each fill gets Settled after settle_delay (config, default 2 s) or SettleFailed with probability p_settle_fail (V18).
- V29 Faults act on reports only (the venue's matching is unaffected): drop (V9 acks, V10 fills), duplicate (V11), hold during a disconnect window (V14) then deliver in order. Each decision is one Philox draw addressed by (seed, report index, purpose).
- V30 Merge/split (D17) are not venue orders: ledger operations after a configurable latency, owned by the engine.

## X Arb executor (src/exec/arb_exec.hpp)

- X1 Trigger: scanner window. Pair groups: buy and sell side (D17). Event groups: buy side only (D10).
- X2 Filters (all must pass; count each rejection by filter): window age >= A_min ms; every leg book updated within S_fresh ms; no leg token frozen; net edge >= E_min.
- X3 Net edge = gross - sum taker fees - legs * slip_ticks - latency allowance (config, ticks; D12).
- X4 Size = floor_step(min(thinnest leg displayed, group cap D3, cash / set cost)); skip if < venue min.
- X5 Policy P (Parallel): all legs Fak at window px at once. Policy Q (seQuential): thinnest leg Fok first; on fill, rest Fak.
- X6 Completion check after all leg reports or T_leg timeout.
- X7 Incomplete set: complete_cost = buy missing legs at asks walking depth; unwind_cost = sell filled legs at bids walking depth. Choose cheaper if loss <= attempt bound (R10); else hold, freeze group, alert; R15 bounds hold time.
- X8 Log per attempt: window id, policy, legs filled, cause (fault id or market), action, cost, net PnL, capital-days.
- X9 One attempt per group at a time; no new attempt while a group has residual exposure.
- X10 Pair completion: buy side merges the set (L6) when both legs are confirmed or matched per config; sell side splits first (L6) then sells both legs; a failed sell leg leaves inventory to unwind (X7).
- X11 Batches: event legs in batches of 15 (D19); policy P sends batches back to back.
