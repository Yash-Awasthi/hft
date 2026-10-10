#include <benchmark/benchmark.h>

#include "core/philox.hpp"

namespace {

using namespace hft::rng;

// 8 blocks (32 words) per iteration.
void BM_PhiloxScalar(benchmark::State& state) {
    std::uint32_t e = 0;
    for (auto _ : state) {
        for (std::uint32_t l = 0; l < 8; ++l)
            benchmark::DoNotOptimize(philox({e, l, 0, 0}, {1, 2}));
        ++e;
    }
    state.SetItemsProcessed(state.iterations() * 32);
}
BENCHMARK(BM_PhiloxScalar);


}  // namespace
