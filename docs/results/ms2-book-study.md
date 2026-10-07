# MS2: order book study

All books produce identical BBO streams on every symbol of 2025-11-28 (12,076 symbols, 353M
messages; seven implementations against `std::map`) and of 2025-12-08 (12,103 symbols, 650M
messages; tick book against `std::map`). An independent Python book agrees on 201 sampled
symbols plus GOOGL.

## Caveats

- Wall-clock figures are provisional: WSL2, no fixed frequency, turbo on, no power-plan
  control. They will be rerun once the baseline conditions of DESIGN.md section 9 are set.
- The WSL2 guest exposes no hardware PMU, so instruction and cache figures come from
  Cachegrind (simulated caches, no hardware prefetcher). Its last-level misses include about
  0.5 per event from streaming the pre-decoded event array, which hardware prefetches.
- Cachegrind runs use the ten busiest symbols (30.6M events); timed runs use the fifty
  busiest (70.4M events) or GOOGL alone (5.2M events).
- Per-event latency wraps each update in `lfence; rdtsc` and `rdtscp; lfence`, which stops
  overlap between events; timer overhead (38 ticks) is subtracted. Batch cost runs the same
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
| map | 195.7 [193.0, 212.4] | 165.2 [160.2, 181.4] | 803.4 [779.5, 884.4] | 1386.7 [1278.9, 1656.7] | 0.00018 | 575 | 7.92 | 0.966 | 0 |
| tick | 45.5 [45.0, 46.9] | 43.5 [43.5, 44.9] | 299.4 [298.2, 308.9] | 438.0 [435.6, 458.0] | - | 243 | 2.94 | 0.695 | 0 |
| tick-prefetch | 44.0 [43.7, 44.7] | 35.0 [35.0, 35.1] | 269.3 [267.8, 270.4] | 381.8 [379.2, 382.8] | 0.001 | - | - | - | 0 |
| tick-rh | 46.2 [45.8, 46.9] | 44.2 [43.9, 44.2] | 300.8 [299.6, 302.5] | 444.0 [442.0, 445.6] | 0.19 | 267 | 2.92 | 0.694 | 0 |
| tick-dm | 44.0 [43.6, 44.5] | 41.4 [41.0, 41.4] | 295.4 [292.5, 297.5] | 450.9 [444.8, 452.0] | 0.0017 | 248 | 3.54 | 0.622 | 0 |
| tick-aos | 45.6 [45.3, 45.7] | 43.5 [43.5, 44.2] | 302.5 [301.8, 303.6] | 450.2 [447.0, 452.4] | 1 | 260 | 2.57 | 0.697 | 0 |
| tick-soa | 50.9 [50.6, 52.5] | 51.9 [51.4, 52.8] | 318.0 [315.8, 323.6] | 474.2 [469.4, 481.2] | 0.00018 | 265 | 4.78 | 0.713 | 0 |
| tick-sv | 109.1 [108.5, 112.4] | 90.6 [88.5, 96.5] | 462.1 [457.6, 480.0] | 1046.0 [1026.3, 1090.5] | 0.00018 | 1558 | 14.34 | 0.692 | 0 |
| btree | 102.4 [102.1, 102.9] | 93.5 [93.5, 94.2] | 383.1 [382.8, 383.9] | 566.7 [565.8, 568.3] | 0.00018 | 557 | 3.78 | 0.689 | 0 |
| tick-4k | 47.7 [47.3, 48.0] | 45.7 [45.3, 45.7] | 308.9 [308.2, 309.6] | 454.9 [452.3, 458.0] | 0.0017 | 243 | 2.94 | 0.697 | 0 |

## GOOGL alone (2025-11-28)

| Book | Batch ns/event | p50 ns | p99 ns | p99.9 ns | vs tick (Mann-Whitney p) | Instr/event | D1 miss/event | LL miss/event | Errors |
|---|---|---|---|---|---|---|---|---|---|
| map | 97.2 [92.5, 97.9] | 92.4 [90.2, 92.8] | 305.9 [270.1, 313.8] | 618.5 [508.0, 649.3] | 0.00018 | - | - | - | 0 |
| tick | 26.1 [26.0, 26.4] | 26.8 [26.4, 26.8] | 80.6 [79.2, 81.8] | 198.4 [179.2, 211.5] | - | - | - | - | 0 |
| tick-prefetch | 28.4 [28.2, 28.5] | 25.7 [25.7, 25.7] | 73.8 [73.8, 74.2] | 126.4 [124.9, 128.4] | 0.00018 | - | - | - | 0 |
| tick-rh | 25.9 [25.8, 26.1] | 26.4 [26.4, 26.6] | 80.8 [79.9, 81.7] | 186.9 [178.1, 202.6] | 0.021 | - | - | - | 0 |
| tick-dm | 25.9 [25.8, 26.2] | 25.3 [25.3, 25.7] | 76.2 [74.8, 77.4] | 195.9 [191.6, 206.9] | 0.076 | - | - | - | 0 |
| tick-aos | 26.6 [26.5, 26.8] | 26.8 [26.8, 26.8] | 80.9 [79.2, 82.0] | 201.2 [183.9, 216.8] | 0.011 | - | - | - | 0 |
| tick-soa | 27.5 [27.1, 28.2] | 27.5 [27.3, 27.8] | 86.8 [79.9, 102.2] | 242.1 [196.7, 314.3] | 0.001 | - | - | - | 0 |
| tick-sv | 91.7 [91.4, 91.9] | 59.2 [58.9, 59.2] | 360.5 [359.6, 361.0] | 1025.2 [1009.9, 1037.0] | 0.00018 | - | - | - | 0 |
| btree | 67.7 [67.6, 67.8] | 59.2 [58.9, 59.2] | 176.6 [175.9, 177.3] | 285.0 [280.1, 292.0] | 0.00018 | - | - | - | 0 |
| tick-4k | 26.3 [26.0, 26.5] | 27.0 [26.8, 27.1] | 83.7 [82.0, 84.9] | 187.8 [180.7, 210.1] | 0.79 | - | - | - | 0 |

## Read, decode and replay

decode: 70945362 messages, 10 reps, 33.7 [33.0, 33.9] M msg/s, 2.103 [2.091, 2.149] s
replay: 147565380 messages, 5 reps, 8.9 [8.8, 8.9] M msg/s, 16.652 [16.576, 16.804] s

Read + decode is bound by the single read-ahead zstd thread: decompression alone reaches 37M
messages/s (1.47 GB/s at 41 bytes per stored message); the decode thread alone, with all
chunks already decompressed, reaches 50.4M messages/s. The replay line is the full day of
2025-12-08 for the fifty busiest symbols: read, decode, book and BBO on one thread.

## Targets (DESIGN.md section 9)

| Target | Value | Measured | Status |
|---|---|---|---|
| Read + decode | >= 50M msg/s | 33.7M (pipeline), 50.4M (decode thread alone) | miss / met, see Needs you |
| Book update median | <= 30 ns | 26.8 ns one symbol; 43.5 ns fifty interleaved | met / miss |
| Book update p99 | <= 150 ns | 80.6 ns one symbol; 299 ns fifty interleaved | met / miss |
| Full-day replay, 50 stocks | <= 60 s | 16.7 s (2025-12-08) | met |
| Fork, 10k orders | <= 100 us, >= 10 GB/s | 22.9 us, 32 GiB/s, 789 KB state | met |
