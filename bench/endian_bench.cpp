#include <benchmark/benchmark.h>

#include "core/endian.hpp"

static void BM_LoadBe32(benchmark::State& state) {
    unsigned char bytes[] = {0x01, 0x02, 0x03, 0x04};
    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
        benchmark::DoNotOptimize(hft::load_be32(bytes));
    }
}
BENCHMARK(BM_LoadBe32);
