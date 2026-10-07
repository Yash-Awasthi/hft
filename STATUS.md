# Status

Working log for the milestones in DESIGN.md. Updated at every milestone and whenever a
task finishes or blocks.

## Done

- MS0: toolchain, presets, vcpkg, hosted CI.
- MS1: ITCH decoder, fuzz target, per-symbol zstd store, ingest, independent C counter.

## In progress

### MS2 Order book study and invariants

- [x] Reference `std::map` book with BBO stream and invariant checker; known-answer tests
- [x] Main book: tick-indexed window + bitmap + overflow levels, hot/cold orders,
      index-addressed pools (mmap, THP, populate), open-addressing order-ID map
- [x] Differential tests: random sequences (RapidCheck) and store slices, identical BBO
- [x] Replay app over the store: invariants, share conservation, persistent-cross check
- [x] Independent Python reference book compared on sampled symbols
- [x] Order-ID map variants: linear probing, Robin Hood, direct-mapped window + fallback
- [x] Order layout variants: hot/cold, AoS, SoA
- [x] Counting allocator test: zero heap allocation after startup
- [x] Timing: rdtscp/lfence, HDR histogram, perf_event_open regions, cachegrind counts
- [ ] Benchmarks: read+decode, book update, full-day 50-stock replay (first baseline done;
      book update misses its target, optimisation in progress)
- [x] Book checkpoints every N events (D2) with verification
- [x] Merged multi-symbol replay source with a read-ahead decompression thread
- [x] Stretch: B-tree and sorted-vector books
- [x] MS1 leftover: field-level comparison against a third-party ITCH parser
- [x] Nightly script (`scripts/nightly.sh`); the runner itself needs registration

### Background

- Ingest of 2025-12-08 to 2025-12-11 (log: `~/data/ingest-dec.log`), about 2.5 MB/s.
- After it finishes: run `checkpoint` on each new store and add it to `ingest_day.sh`
  (the script is not edited while the ingest loop is running it).

## Needs you

- Hardware counters: the WSL2 guest has no PMU (`dmesg`: "unsupported CPU family 6 model
  183 no PMU driver, software events only"; `arch_perfmon` missing from `/proc/cpuinfo`).
  Instruction counts and cache simulation come from Cachegrind until this is fixed.
  Check `hardwarePerformanceCounters=true` under `[wsl2]` in `.wslconfig`, then
  `wsl --shutdown`.
- Self-hosted nightly runner registration (repository settings and token).
- Wall-clock benchmark baseline: needs mains power and maximum processor state 99%.

## Results

| Milestone | Metric | Target | Measured | Evidence |
|-----------|--------|--------|----------|----------|
| MS2 | BBO stream, 7 books vs std::map, 2025-11-28, all 12,076 symbols | identical | identical (per-symbol FNV hashes, 353M messages): tick, tick-rh, tick-dm, tick-aos, tick-soa, sorted vector, B-tree | `book_replay <store> <book>` |
| MS1 | Fields vs itchfeed 1.6.4 (third party) | identical | 12M messages in 5 slices (open, mid-day, cross, close), 0 mismatches | `research/compare_itchfeed.py` |
| MS1 | Merged store stream vs decompressed download | identical | SHA-256 e115c7e4... matches `zcat` | `store_cat <store> \| sha256sum` |
| MS2 | Python reference vs C++ on sampled symbols | identical | 201 sampled + GOOGL (5.2M messages) identical | `research/compare_ref.py <store> <table> 200 2` |
| MS2 | Invariants every event | 0 failures | 0 on 255M book events (11,778 symbols); 0 at every 1,000 events on all symbols | `CHECK_EVERY=1 book_replay` |
| MS2 | Persistent cross in trading state | none over 100 ms | longest 1.4 ms (SMX, post-halt uncross) | `book_replay` max_cross_ms |
| MS2 | Share conservation per symbol | exact | exact on all symbols | `book_replay` check_fail |
| MS2 | Heap allocations in steady state | 0 | 0 new, 0 pool maps over 500k messages | `alloc_test` |
| MS2 | Checkpoints (every 1M messages) | byte-identical replay | 71 checkpoints, all verified, 57 MB | `checkpoint <store> --verify` |
