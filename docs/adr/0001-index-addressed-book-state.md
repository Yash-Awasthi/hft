# 1. Index-addressed book state with a window and a radix tree for price levels

## Status

Accepted, 2026-10-07.

## Context

The book must fork by `memcpy` (C4, N2), avoid heap allocation after start-up and update in
tens of nanoseconds. archive/docs/DESIGN.md section 9 specifies a dense window of price levels around the
mid with a bitmap, and an overflow map for prices far outside it.

On 2025-11-28 the busiest symbols keep thousands of levels outside a 2,048-tick window
(GOOGL: 6,117 on average, NVDA: 9,290). A sorted array as the overflow map made `memmove`
57% of all instructions (670 instructions per event, Cachegrind, ten busiest symbols).

## Decision

- Orders, levels and tree nodes live in pools addressed by 32-bit indices, backed by
  anonymous mappings with transparent huge pages; a fork copies the used part of each pool.
- Price levels near the touch use the window. Levels on the tick grid outside it use a
  three-level radix tree of 256-way nodes keyed by tick index, with a bitmap at each level,
  so the best deep level is found with a few `lzcnt`/`tzcnt` and inserts never move data.
  Prices off the grid (rare: a stock crossing $1 after its tick was chosen) keep a small
  sorted array.
- A level is located once per operation and the result reused for the update and for
  removing an emptied level.

## Consequences

Instructions per event fell from 670 to 262 on the same workload, and GOOGL alone runs at
26 ns per event in batch. Deep levels cost one 4 KB page per 256 ticks touched, which adds to
the fork size of symbols with orders spread far from the touch.
