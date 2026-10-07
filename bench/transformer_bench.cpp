#include <benchmark/benchmark.h>

#include <cstdlib>
#include <string>

#include "strategy/event_transformer.hpp"

namespace {

using hft::strategy::EventToken;
using hft::strategy::EventTransformer;

// Weights from HFT_TRANSFORMER (a trained model) or the test fixture. The window is filled
// first, so every step attends over the full ring.
template <bool Avx>
void BM_EventStep(benchmark::State& state) {
    const char* env = std::getenv("HFT_TRANSFORMER");
    const EventTransformer m(env ? std::string(env) : std::string(HFT_SOURCE_DIR "/tests/data/evt_weights.bin"));
    auto s = m.state();
    float f[16], g[EventTransformer::kGen];
    int i = 0;
    for (; i < 512; ++i) m.step<Avx>(s, {i % 5, i % 2, i % 10, i % 8, 1.5f}, f, g);
    for (auto _ : state) {
        m.step<Avx>(s, {i % 5, i % 2, i % 10, i % 8, 1.5f}, f, g);
        benchmark::DoNotOptimize(f);
        benchmark::DoNotOptimize(g);
        ++i;
    }
    state.counters["weight_KB"] = static_cast<double>(m.weight_bytes()) / 1024;
}
BENCHMARK(BM_EventStep<false>)->Name("EventStep/scalar");
BENCHMARK(BM_EventStep<true>)->Name("EventStep/avx2");

}  // namespace
