#include <benchmark/benchmark.h>

#include "core/philox.hpp"

namespace {

using namespace hft::rng;

// 8 blocks (32 words) per iteration in both cases.
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

void BM_PhiloxAvx2(benchmark::State& state) {
    if (!has_avx2()) {
        state.SkipWithError("no AVX2");
        return;
    }
    Lanes in{}, out;
    for (std::uint32_t l = 0; l < 8; ++l) in.v[1][l] = l;
    for (auto _ : state) {
        philox_x8(in, {1, 2}, out);
        benchmark::DoNotOptimize(out);
        for (auto& c : in.v[0]) ++c;
    }
    state.SetItemsProcessed(state.iterations() * 32);
}
BENCHMARK(BM_PhiloxAvx2);

}  // namespace
