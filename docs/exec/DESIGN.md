# Design

## T Types (src/exec/types.hpp)

- T1 `Px` int32 1e-4; `Qty` int64 (unit D5); `Usd` int64 micro-dollars (D5); `Ns` int64.
- T2 `Side {Buy, Sell}`; `Tif {Gtc, Fok, Fak}` (confirm Q3).
- T3 `OrderIntent {token u32, side, px, qty, tif, strategy u16, tag u32}`.
- T4 `VenueReq {kind New|Cancel, cl_id u64, token, side, px, qty, tif}`.
- T5 `VenueRpt {kind Ack|Reject|Fill|CancelAck|CancelReject|Expired|Status, cl_id, venue_id u64, fill_id u64, px, qty, fee Usd, reason u16, venue_ns}`.
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

- S15 Fill application is by quantity, idempotent by fill_id (small per-order ring of recent fill ids, size 8; overflow -> linear check against ledger fills for that order).
- S16 Transition table is constexpr data; one function applies it; O(1), no alloc.
- S17 Budget: transition <= 50 ns p99 (bench).

## R Risk (src/exec/risk.hpp)

Pre-trade, order (cheapest first; first failure returns its reason):

- R1 kill switch off; token not frozen (Unknown order, closed market, tick change pending, feed stale).
- R2 px on token tick grid, 1 <= px <= 9999; qty multiple of size step, >= venue min (Q5).
- R3 collar: buy px <= best_ask + C ticks; sell px >= best_bid - C ticks; requires two-sided fresh book.
- R4 self-cross: no buy >= own resting sell on same token, no sell <= own resting buy.
- R5 position incl. open orders, worst side: per token qty cap; per market notional cap (D3); per group cap (D3); gross cap (D3).
- R6 cash: reserved buys + this order notional + fees <= available cash.
- R7 token buckets: order rate and cancel rate, per token and global, at <= 80% of venue limit (Q7).
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

- L1 Per token: pos Qty, cost Usd, realised Usd, fees Usd. Per group: complete sets held (min over legs of pos).
- L2 Identity after every fill and mark: total = realised + unrealised + fees (exact int).
- L3 Capital: locked_in_orders, locked_in_positions; capital_days += locked * dt each tick.
- L4 Resolution: settle each token at 0 or 10000; sets settle at 10000 per set.
- L5 Overflow guard: static_assert / debug check on max notional (D5 headroom).

## V SimVenue (src/exec/sim_venue.hpp)

Matching:

- V1 Request arrives at t_send + L_in; report leaves venue at t_event and arrives at + L_out. L from config: fixed or empirical sample (D4); Philox draw per message (purpose ids distinct).
- V2 Taker: on arrival, walk displayed levels at arrival time; fill at each level price up to displayed - overlay[level]. overlay[level] += taken; cleared when a market update for that level arrives.
- V3 Maker: rest at px; ahead = displayed size at arrival (0 if px improves best). Trade at px reduces ahead, then fills; trade through px fills all remaining; level shrink below ahead -> ahead = level size. Cancels ahead not credited. (Same as pm/maker.hpp today; G3b parity.)
- V4 Tif: Fok all-or-none at arrival; Fak fill available, cancel rest (Expired report); Gtc rests.
- V5 Fees per Q6 on each fill.
- V6 Rule rejects: off-tick, below min, closed/resolved market, paper cash insufficient, px out of bounds.
- V7 Market events: tick change -> reject resting orders off new grid (or cancel all on token, per Q4); close/resolve -> cancel all, settle.

Faults (each config prob or schedule; Philox, seed in exec.cfg):

- Core (D14): V9 drop ack; V10 drop fill report (fill happened; found at reconcile); V11 duplicate report; V14 disconnect T s (reports after reconnect in order, or lost -> reconcile).
- Optional: V8 spurious reject; V12 reorder (cancel ack before earlier fill); V13 delay > OMS timeout.

Invariants (checked every event; abort in tests):

- V15 fill qty <= order qty; buy fill px <= limit; sell fill px >= limit.
- V16 taken from level <= displayed + overlay accounting.
- V17 venue positions == sum of fills; venue cash == ledger cash at reconcile.

Limits (print in every report): L2 has no order ids (queue estimated); others do not react to us; latency modelled.

## X Arb executor (src/exec/arb_exec.hpp)

- X1 Trigger: scanner window, buy side only (D10).
- X2 Filters (all must pass; count each rejection by filter): window age >= A_min ms; every leg book updated within S_fresh ms; no leg token frozen; net edge >= E_min.
- X3 Net edge = gross - sum taker fees - legs * slip_ticks - latency allowance (config, ticks; D12).
- X4 Size = floor_step(min(thinnest leg displayed, group cap D3, cash / set cost)); skip if < venue min.
- X5 Policy P (Parallel): all legs Fak at window px at once. Policy Q (seQuential): thinnest leg Fok first; on fill, rest Fak.
- X6 Completion check after all leg reports or T_leg timeout.
- X7 Incomplete set: complete_cost = buy missing legs at asks walking depth; unwind_cost = sell filled legs at bids walking depth. Choose cheaper if loss <= attempt bound (R10); else hold, freeze group, alert; R15 bounds hold time.
- X8 Log per attempt: window id, policy, legs filled, cause (fault id or market), action, cost, net PnL, capital-days.
- X9 One attempt per group at a time; no new attempt while a group has residual exposure.
