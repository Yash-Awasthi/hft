# Build and runtime measurements

Machine and conditions: HW.md. Each entry: id, what, before -> after, command or file, commit.

## Build (2026-10-10)

- P1 Clean build, ccache off, 20 jobs: release 36.1 s -> 32.6 s; native (LTO) 25.6 s -> 23.7 s (d16c31c).
- P2 Cost is code generation more than parsing: pm_test.cpp alone 16.7 s full vs 4.2 s parse (-fsyntax-only); RapidCheck/gtest templates at -O3 dominate test files.
- P3 Header weight (preprocessed lines on their own): core/spsc.hpp 137k -> 29k (sched_yield and __builtin_ia32_pause instead of <thread> and <immintrin.h>); net/tape.hpp 110k -> 36k (GCC vector types and builtins instead of <immintrin.h>, realloc instead of <memory>); exec/oms.hpp 96k -> 34k (pm/rules.hpp split from pm/gamma.hpp, which pulled OpenSSL, sockets and the JSON reader); core/tsc.hpp: builtins and clock_gettime instead of <x86intrin.h> and <chrono>. apps/pm_live.cpp 224k -> 152k. Measure: build-free script, one header per translation unit, g++ -E | wc -l.
- P4 Heaviest standard headers measured: <chrono> 90k lines / 0.98 s, <filesystem> 80k, <thread> 77k, <fstream>/<sstream>/<iostream> ~70k each, <immintrin.h> 55k, <memory> 56k. Kept out of hot-path headers; still used in file I/O (pm/reader.hpp 104k, pm/session.hpp 142k) and apps.
- P5 Edit loop (touch a header, rebuild, ccache off): exec/oms.hpp 7.8 s; pm/msg.hpp 17.3 s -> 15.0 s after splitting pm_test.cpp (decoder tests to pm_decoder_test.cpp). Bound by the slowest dependent test file.
- P6 Tests at -O1 instead of -O3 (release preset): msg.hpp edit 15.0 -> 11.7 s, oms.hpp 7.8 -> 6.0 s, test run unchanged (2.9 s). Not adopted: release tests would no longer exercise -O3 code generation of header-only product code. Owner can choose.
- P7 Linkers: GCC LTO needs a GCC-plugin linker (bfd/gold); lld is installed but cannot do GCC LTO; mold not installed. Release links are small; native link of hft_tests ~10 s is LTO code generation, not linking.
- P8 Dead code removed: Philox AVX2 eight-lane path (no production caller; perf notes found it latency-bound and unused).

## Runtime (2026-10-10)

- R1 pm_live --replay (2.0M msgs, native, vCPUs 2,4): 1.92 s wall, RSS 35 MB, 8.4k minor faults (setup; ~1% of runtime). parse p50 538 ns, engine 177 ns.
- R2 SpscBytes first lap (8 MB ring, 600-byte records, consumer keeping up): p99 1.8 us, p99.9 3.4-6.4 us, max 71-164 us per write (page faults) -> Pool mapping (2 MB pages, pre-faulted): p99 104-136 ns, p99.9 223-244 ns, max mostly < 21 us (7f6a0ad). Affects live feed threads and the recorder ring, not replay.
- R3 C++26 (-std=c++26, GCC 15.2): decoder 517-525 ns/msg either standard (noise). libstdc++ 15 has no <inplace_vector> or <simd>; <experimental/simd> (TS) exists. No reason to switch now.
- R4 simdjson 3.13 On-Demand (haswell kernel) on the same 300k messages, same fields and the same price/size parsers: 430-443 ns/msg (1.41-1.45 GB/s) vs ours 504-516 ns/msg (1.21-1.24 GB/s); simdjson rejected 2 messages ours decodes. ~15% faster, but 7.6 MB of source (compile time on every includer), needs padded buffers, validates only what is read. Not adopted; if parse speed matters more, walk the tape's SIMD structural index in the single pass instead (its technique, not its code). Bench: ~/scratch/sj_bench.cpp.
- R5 Allocators (mimalloc, jemalloc): no effect possible on the trading path; steady state allocates nothing (alloc_test). Not tried.
