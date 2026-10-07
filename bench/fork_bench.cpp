#include <benchmark/benchmark.h>

#include <random>

#include "book/tick_book.hpp"

namespace {

using Book = hft::book::TickBook<>;

// 10k resting orders: most within 50 ticks of a $100 mid, a tenth spread far out.
void fill(Book& b, std::size_t n) {
    std::mt19937_64 rng(1);
    for (std::uint64_t ref = 1; ref <= n; ++ref) {
        const bool sell = rng() % 2;
        const std::uint32_t ticks = rng() % 10 == 0 ? 50 + rng() % 5000 : 1 + rng() % 50;
        const std::uint32_t px = sell ? 100'0000 + ticks * 100 : 100'0000 - ticks * 100;
        b.add(ref, sell ? hft::book::Side::Sell : hft::book::Side::Buy, 100, px, ref);
    }
}

void BM_Fork(benchmark::State& state) {
    Book src(16384), dst(16384);
    fill(src, static_cast<std::size_t>(state.range(0)));
    dst.copy_from(src);  // first copy faults in the destination
    for (auto _ : state) {
        dst.copy_from(src);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * src.state_bytes()));
    state.counters["state_bytes"] = static_cast<double>(src.state_bytes());
}
BENCHMARK(BM_Fork)->Arg(10'000)->Unit(benchmark::kMicrosecond);

}  // namespace
