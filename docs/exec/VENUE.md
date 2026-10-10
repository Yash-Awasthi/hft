# Venue facts (Polymarket CLOB)

Read 2026-10-10 from docs.polymarket.com unless noted. Facts change: re-read before relying on a number.
Status per fact: V = verified from docs; M = measured here; U = uncertain or conflicting (note says why).

## Orders (answers Q3, Q5)

- F1 V Order types: GTC, GTD (limit); FOK, FAK (market). All orders are limits; a market order is a marketable limit. FAK is the default for market orders. Source: /trading/orders/create, /trading/orders/overview.
- F2 V Post-only: GTC/GTD only; rejected if it would match. -> maker quotes post-only GTC.
- F3 V GTD expiry: unix seconds, expires 1 min early (use now+60+N), must be >= 3 min out. Market orders expiration "0".
- F4 V Limit size = shares; market BUY size = USD amount (pre-fee); market SELL = shares. Price/guard: maxPrice (buy), minPrice (sell).
- F5 V Signed amounts are 6-decimal integers (1 USD = 1_000_000; 1 share = 1_000_000).
- F6 V Precision by tick: tick 0.1 -> price 1 dp, size 2 dp, amount 3 dp; 0.01 -> 2/2/4; 0.005 -> 3/2/5; 0.0025 -> 4/2/6; 0.001 -> 3/2/5; 0.0001 -> 4/2/6. Limit: size rounded down to 2 dp; amount: round up to (amount dp + 4) then down to amount dp; recheck min size after rounding.
- F7 V Valid tick sizes: 0.1, 0.01, 0.001, 0.0001 (precision table also lists 0.005, 0.0025). Off-tick price -> reject.
- F8 V Min order size per market: book field `min_order_size` (example "5"; unit assumed shares, U: docs do not state unit).
- F9 V Placement response: success, errorMsg, orderID, status in {live, matched, delayed, unmatched}, makingAmount, takingAmount, tradeIDs, transactionsHashes. Known error text: "not enough balance / allowance".
- F10 V Sports markets: marketable orders get a 1 s matching delay (status `delayed`); if unmatched after the delay the order rests (`unmatched`). Source: /concepts/order-lifecycle.
- F11 V Batch: 1..15 orders per POST /orders. Batch cancel up to 3000 ids.
- F12 V Modes: cancel-only (new orders rejected, cancels allowed); closed-only (position-opening orders rejected).
- F13 V Heartbeat dead-man: send every 5 s; none valid within 10 s -> all open orders for the credentials cancelled (up to +5 s).

## Trades and settlement (answers Q8)

- F14 V Matching off-chain (CLOB), settlement on-chain (Polygon, CTF Exchange). Source: /concepts/order-lifecycle; arXiv 2606.04217.
- F15 V Trade status: MATCHED -> MINED -> CONFIRMED (terminal); RETRYING loops back to MINED; FAILED terminal. Newer schema adds MATCHED_NOT_BROADCASTED; set is not fixed. A matched trade can fail.
- F16 V User channel (authenticated) sends `order` events (PLACEMENT, UPDATE = partial fill, CANCELLATION) and `trade` events with status changes. Fill before ack: not stated; treat as possible (DESIGN S4). Trade ids exist (tradeIDs) for dedup.

## Fees (answers Q6)

- F17 V Takers only; makers never pay. fee = C * feeRate * p * (1 - p), C = shares, p = price; charged at match time in USDC; rounded to 5 dp; min charged 0.00001 USDC. Source: /trading/fees.
- F18 V feeRate by category: crypto 0.07; sports 0.05; finance, politics, mentions, tech 0.04; economics, culture, weather, other 0.05; geopolitics 0 (fee-free). Maker rebate 15-25% of fees by category (paid daily; not modelled in v1).
- F19 U One source shows fee = C * p * feeRate * (p(1-p))^exponent with per-category exponents (crypto exp 2, sports 0.03 exp 1, March 2026 schedule). Current fees page has no exponent. Use F17; make the formula a config switch.
- F20 V Per-market fee check: GET clob.polymarket.com/fee-rate?token_id=...

- F20b V Gamma market fields (2026-10-10, 40 top events): feeType (e.g. sports_fees_v3, politics_fees, crypto_fees_v2, culture_fees, weather_fees, finance_prices_fees, economics_fees, sports_fees_nfl_cfb_oct26), feeSchedule {exponent 1, rate 0.03-0.07, takerOnly true, rebateRate 0.15-0.25}, feesEnabled false on some (e.g. geopolitics, an election), secondsDelay 1 on sports game markets, orderMinSize 5, orderPriceMinTickSize 0.001 or 0.01. -> D24.

## Rate limits (answers Q7)

- F21 V Per signer, sliding windows: POST /order 5000/10 s burst, 48000/10 min sustained; DELETE /order same; POST /orders 1500/10 s, 21000/10 min; DELETE /orders 1000/10 s (changelog says 2000, U), 15000/10 min; DELETE /cancel-all 250/10 s, 6000/10 min. Tiered by 30-day maker volume. Source: /api-reference/trading-rate-limits.

## Positions, split/merge, resolution (answers Q9, Q10)

- F22 V Each binary market: YES and NO tokens (ERC-1155, CTF); each pair backed by exactly 1 pUSD.
- F23 V Split: 1 pUSD -> 1 YES + 1 NO. Merge: 1 YES + 1 NO -> 1 pUSD. Merge prerequisites do not include resolution (CTF mergePositions works any time; U: docs do not say so explicitly). Submitted via relayer ("gasless"; U: who pays gas not stated).
- F24 V Neg-risk events (`neg_risk` true): one NO in one market converts atomically to one YES in every other market (Neg Risk Adapter). Augmented neg-risk: trade only named outcomes; avoid "Other".
- F25 V Resolution: UMA optimistic oracle; proposal bond ~750 pUSD; 2 h challenge; disputes escalate to UMA DVM vote. Winning token redeems 1 pUSD, losing 0; no redemption deadline.
- F26 V Feed messages seen in recordings: `market_resolved` (fields: market, assets_ids, winning_asset_id, winning_outcome, timestamp, tags), `tick_size_change` (asset_id, old_tick_size, new_tick_size; observed 0.01 -> 0.001), `new_market`, `best_bid_ask`, `book`, `price_change`, `last_trade_price`.

## Measured here (answers Q11)

- F27 M TCP connect to clob.polymarket.com (Cloudflare edge, BOM): median 66.9 ms, p90 74.8 ms (n=20).
- F28 M Request round trip after TLS (GET /time, edge + origin): median 181.6 ms, p90 299.5 ms, min 161.5 ms (n=20). -> measured RTT setting = 182 ms (D4).
- F29 M Feed: receive_ns - exchange `timestamp` on 276,896 messages: p1 -216 ms, p50 -153 ms, p90 +2.68 s, p99 +9.8 s. Local clock synced (chrony stratum 1, PHC0). Exchange timestamps are not comparable with local time (negative median) and snapshots carry old timestamps: never use them for latency or ordering.
- F30 M Event counts in all recordings (pm 278 MB + pm-live 176 MB): price_change 4.44M, best_bid_ask 269k, book 95k, new_market 51k, last_trade_price 46.6k, market_resolved 28, tick_size_change 24, _reconnect 9.

- F31 M Arb baseline, `pm_arb ~/data/pm-live` (4 runs, ~11 h, 2026-10-09..10; windows >= 1 ms): 298 windows, 11 groups.
  asks<1: n 261, duration p50 244 ms / p90 33 s, max edge p50 0.0040 / p90 0.0140 / max 0.0710, size at max p50 7.2 / p90 14.8 shares.
  bids>1: n 37, duration p50 1.0 s / p90 32 s, max edge p50 0.0020 / p90 0.0300, size p50 5.2 / p90 50 shares.
  At RF2's threshold (edge >= 0.02, lasting >= 1 s): 17 windows, ~10 USD gross total at displayed size, before fees and latency.
  -> depth, not capital, bounds results (D3 caps never bind); E6 should report this honestly; more recording days raise counts, not per-window size.

## Consequences for the design (applied in DECISIONS D16-D20)

- Settlement can fail after a match (F15) -> ledger separates matched from confirmed; a settlement-failure fault is core.
- Sports 1 s delay (F10) -> SimVenue per-market matching delay; arb legs in sports groups face it.
- Merge/split (F23) -> Yes/No pair windows: buy side frees capital at once by merging; sell side executable by splitting.
- Fee formula p(1-p) by category (F17, F18) -> session needs each market's category.
- Batch cap 15 (F11) -> event-level arb with more than 15 legs needs several batches (more skew).
- Min size and 2-dp sizes (F6, F8) -> sizing rounds down to 0.01 share and skips below min.
