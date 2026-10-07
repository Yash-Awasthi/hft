# MS2: order book study

All books produce identical BBO streams on every symbol of 2025-11-28 (12,076 symbols, 353M
messages; seven implementations against `std::map`) and of 2025-12-08 (12,103 symbols, 650M
messages; tick book against `std::map`). An independent Python book agrees on 201 sampled
symbols plus GOOGL.

## Caveats

- Wall-clock figures are the baseline of DESIGN.md section 9: laptop on mains, idle,
  maximum processor state 99% for every core class and boost mode off in the Windows power
  plan (turbo off; a chain of dependent multiplies runs at about 2.4 GHz on every virtual CPU
  tried, against 4.7 GHz before). Timed runs pinned with `taskset` (one virtual CPU; two for
  modes with the read-ahead thread). WSL2 still cannot fix frequency or core placement.
- The WSL2 guest exposes no hardware PMU, so instruction and cache figures come from
  Cachegrind (simulated caches, no hardware prefetcher). Its last-level misses include about
  0.5 per event from streaming the pre-decoded event array, which hardware prefetches.
- Cachegrind runs use the ten busiest symbols (30.6M events); timed runs use the fifty
  busiest (70.4M events) or GOOGL alone (5.2M events).
- Per-event latency wraps each update in `lfence; rdtsc` and `rdtscp; lfence`, which stops
  overlap between events; timer overhead (71 TSC ticks, 25 ns) is subtracted. Batch cost runs the same
  events untimed. The prefetch variant prefetches the ID slot of the event 16 ahead outside
  the timed region, so only its batch figure is comparable.
- Intervals are 95% bootstrap intervals of the median over 10 repetitions after one warm-up;
  p-values are two-sided Mann-Whitney tests of batch cost against `tick`.

## Variants

| Name | Price levels | Orders | Order-ID map |
|---|---|---|---|
| map | `std::map` per side, `std::list` queues | node per order | `std::unordered_map` |
| tick | 2,048-tick window + radix tree for deep levels | hot/cold split (16 + 24 B) | linear probing, 8 B slots |
| tick-rh | as tick | as tick | Robin Hood |
| tick-dm | as tick | as tick | direct-mapped 4,096 slots + linear fallback |
| tick-aos | as tick | one 40 B record | linear probing |
| tick-soa | as tick | one array per field | linear probing |
| tick-sv | sorted vector per side | hot/cold | linear probing |
| btree | `tlx::btree_map` per side | hot/cold | linear probing |
| tick-4k | as tick, pools on 4 KB pages | as tick | as tick |

## Fifty busiest symbols, interleaved in feed order (2025-11-28)

| Book | Batch ns/event | p50 ns | p99 ns | p99.9 ns | vs tick (Mann-Whitney p) | Instr/event | D1 miss/event | LL miss/event | Errors |
|---|---|---|---|---|---|---|---|---|---|
| map | 333.5 [333.1, 335.7] | 315.0 [313.9, 316.1] | 1220.4 [1214.0, 1225.8] | 2620.9 [2600.9, 2655.2] | 0.00018 | 575 | 7.92 | 0.966 | 0 |
| tick | 74.7 [74.1, 74.8] | 81.0 [80.1, 81.0] | 365.6 [363.5, 366.7] | 575.5 [571.5, 577.9] | - | 243 | 2.94 | 0.695 | 0 |
| tick-prefetch | 73.5 [72.7, 74.3] | 68.1 [67.2, 68.1] | 338.1 [330.0, 339.6] | 495.5 [486.8, 507.1] | 0.011 | - | - | - | 0 |
| tick-rh | 76.2 [75.5, 77.1] | 81.8 [81.3, 82.4] | 372.0 [370.4, 374.4] | 602.5 [592.2, 608.9] | 0.00018 | 267 | 2.92 | 0.694 | 0 |
| tick-dm | 74.6 [74.2, 76.1] | 79.2 [78.8, 79.9] | 370.0 [368.2, 373.1] | 588.6 [584.0, 593.3] | 0.62 | 248 | 3.54 | 0.622 | 0 |
| tick-aos | 75.7 [75.4, 76.9] | 80.7 [80.3, 81.0] | 370.8 [369.6, 372.8] | 592.9 [590.4, 602.5] | 0.00018 | 260 | 2.57 | 0.697 | 0 |
| tick-soa | 83.8 [83.0, 84.9] | 96.8 [96.0, 100.3] | 397.6 [396.0, 407.0] | 630.7 [620.0, 652.5] | 0.00018 | 265 | 4.78 | 0.713 | 0 |
| tick-sv | 194.4 [192.5, 203.7] | 149.3 [148.8, 155.5] | 817.3 [813.7, 858.7] | 1917.5 [1901.8, 2224.2] | 0.00018 | 1558 | 14.34 | 0.692 | 0 |
| btree | 180.0 [179.6, 180.8] | 157.0 [157.0, 157.7] | 630.0 [628.6, 633.6] | 875.1 [870.1, 890.1] | 0.00018 | 557 | 3.78 | 0.689 | 0 |
| tick-4k | 78.2 [77.8, 78.8] | 81.7 [81.0, 81.7] | 377.1 [374.8, 379.2] | 612.9 [589.0, 619.7] | 0.00018 | 243 | 2.94 | 0.697 | 0 |

## GOOGL alone (2025-11-28)

| Book | Batch ns/event | p50 ns | p99 ns | p99.9 ns | vs tick (Mann-Whitney p) | Instr/event | D1 miss/event | LL miss/event | Errors |
|---|---|---|---|---|---|---|---|---|---|
| map | 182.1 [180.7, 188.5] | 175.2 [174.8, 176.9] | 474.9 [465.6, 515.8] | 708.1 [670.7, 816.6] | 0.00018 | - | - | - | 0 |
| tick | 50.9 [50.6, 51.3] | 53.5 [53.1, 53.5] | 152.0 [150.9, 152.3] | 242.9 [241.8, 248.1] | - | - | - | - | 0 |
| tick-prefetch | 54.6 [54.3, 54.7] | 51.4 [51.4, 51.4] | 144.7 [144.1, 144.8] | 225.5 [224.4, 228.0] | 0.00018 | - | - | - | 0 |
| tick-rh | 50.2 [50.1, 50.3] | 52.8 [52.8, 53.5] | 151.8 [151.3, 152.7] | 232.4 [231.5, 235.8] | 0.0013 | - | - | - | 0 |
| tick-dm | 50.0 [49.8, 50.1] | 50.7 [50.7, 50.7] | 144.8 [144.2, 145.2] | 236.8 [234.2, 241.2] | 0.00018 | - | - | - | 0 |
| tick-aos | 50.9 [50.8, 51.3] | 53.5 [53.5, 53.5] | 150.9 [150.2, 152.7] | 244.8 [242.6, 252.0] | 0.85 | - | - | - | 0 |
| tick-soa | 52.1 [52.0, 52.3] | 54.9 [54.9, 54.9] | 154.1 [153.8, 154.8] | 249.3 [248.4, 255.4] | 0.00018 | - | - | - | 0 |
| tick-sv | 178.4 [178.2, 178.8] | 115.9 [115.8, 115.9] | 706.7 [706.7, 707.4] | 2442.6 [2414.0, 2454.0] | 0.00018 | - | - | - | 0 |
| btree | 131.2 [131.0, 131.7] | 115.9 [115.9, 116.3] | 343.0 [342.5, 344.4] | 512.8 [509.1, 515.6] | 0.00018 | - | - | - | 0 |
| tick-4k | 50.5 [50.4, 50.6] | 53.5 [53.5, 53.5] | 153.4 [152.0, 153.8] | 246.7 [242.6, 247.9] | 0.0073 | - | - | - | 0 |

## Read, decode and replay

decode: 70945362 messages, 10 reps, 17.8 [17.6, 18.0] M msg/s, 3.979 [3.951, 4.026] s
decode-only: 70945362 messages, 10 reps, 18.8 [18.6, 19.0] M msg/s, 3.783 [3.742, 3.810] s
decompress: 70945362 messages, 10 reps, 17.4 [17.2, 17.8] M msg/s, 4.087 [3.981, 4.115] s
replay: 147565380 messages, 10 reps, 5.7 [5.7, 5.7] M msg/s, 25.911 [25.812, 25.971] s

`decode` is the pipeline: read-ahead zstd thread plus the merge-and-decode thread.
`decode-only` is the decode thread alone: every chunk decompressed before the clock starts,
then merge into feed order and decode. `decompress` is one thread decompressing every chunk
(41 bytes per stored message, 0.71 GB/s) without decoding. At the baseline the two stages run
at about the same rate, so a second decompression thread would not lift the pipeline much:
the decode thread alone is also short of 50M. The replay line is the full day of 2025-12-08
for the fifty busiest symbols: read, decode, book and BBO on one thread.

The earlier turbo-on figures (pipeline 33.7M, decode thread alone 50.4M, decompression 37M
msg/s) came from ad hoc runs with no recorded tool; `decode-only` and `decompress` replace
them and are not directly comparable.

## Targets (DESIGN.md section 9)

| Target | Value | Measured | Status |
|---|---|---|---|
| Read + decode | >= 50M msg/s | 18.8M (decode thread alone), 17.8M (pipeline) | miss |
| Book update median | <= 30 ns | 53.5 ns one symbol; 81.0 ns fifty interleaved | miss / miss |
| Book update p99 | <= 150 ns | 152.0 ns one symbol; 365.6 ns fifty interleaved | miss / miss |
| Full-day replay, 50 stocks | <= 60 s | 25.9 s (2025-12-08) | met |
| Fork, 10k orders | <= 100 us, >= 10 GB/s | 53.4 us, 16.0 GiB/s, 869 KB state | met |
