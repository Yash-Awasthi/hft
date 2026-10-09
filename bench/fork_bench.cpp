#include <benchmark/benchmark.h>

#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <random>
#include <vector>

#include "book/tick_book.hpp"

namespace {

using Book = hft::book::TickBook;

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

constexpr std::size_t kSymbols = 50;

std::vector<std::unique_ptr<Book>> make_books(std::size_t orders) {
    std::vector<std::unique_ptr<Book>> v;
    for (std::size_t i = 0; i < kSymbols; ++i) {
        v.push_back(std::make_unique<Book>(16384));
        fill(*v.back(), orders);
    }
    return v;
}

// A 50-symbol state forked by copying every book.
void BM_ForkMulti(benchmark::State& state) {
    const auto src = make_books(static_cast<std::size_t>(state.range(0)));
    std::vector<std::unique_ptr<Book>> dst;
    for (std::size_t i = 0; i < kSymbols; ++i) {
        dst.push_back(std::make_unique<Book>(16384));
        dst[i]->copy_from(*src[i]);  // first copy faults in the destination
    }
    for (auto _ : state) {
        for (std::size_t i = 0; i < kSymbols; ++i) dst[i]->copy_from(*src[i]);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ForkMulti)->Arg(10'000)->Unit(benchmark::kMicrosecond);

// The same state forked as a process; with Arg(1) the child also writes to every book.
void BM_ProcessFork(benchmark::State& state) {
    auto books = make_books(10'000);
    const bool touch = state.range(0) != 0;
    for (auto _ : state) {
        const pid_t pid = fork();
        if (pid == 0) {
            if (touch)
                for (auto& b : books) b->add(1ULL << 40, hft::book::Side::Buy, 100, 99'0000, 1);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }
}
BENCHMARK(BM_ProcessFork)->Arg(0)->Arg(1)->Unit(benchmark::kMicrosecond);

}  // namespace
