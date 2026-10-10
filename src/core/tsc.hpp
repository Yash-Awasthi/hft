#pragma once

// Cycle timing with rdtscp. start() fences so earlier work is retired before the read;
// stop() uses rdtscp, which waits for earlier instructions, then fences so later work does
// not begin before the read. Compiler builtins instead of <x86intrin.h> and clock_gettime
// instead of <chrono>: those headers cost every includer ~145k preprocessed lines.

#include <cpuid.h>
#include <time.h>

#include <cstdint>

namespace hft::tsc {

inline std::uint64_t start() {
    __builtin_ia32_lfence();
    const std::uint64_t t = __builtin_ia32_rdtsc();
    __builtin_ia32_lfence();
    return t;
}

inline std::uint64_t stop() {
    unsigned aux;
    const std::uint64_t t = __builtin_ia32_rdtscp(&aux);
    __builtin_ia32_lfence();
    return t;
}

// CPUID 0x80000007 EDX bit 8: the TSC rate does not change with frequency or C-states.
inline bool invariant() {
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000007, &a, &b, &c, &d)) return false;
    return (d >> 8) & 1;
}

inline std::int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// TSC ticks per nanosecond, measured against the monotonic clock over `ms` milliseconds.
inline double ticks_per_ns(int ms = 100) {
    const std::int64_t t0 = mono_ns();
    const std::uint64_t c0 = start();
    while (mono_ns() - t0 < std::int64_t{ms} * 1'000'000) {
    }
    const std::uint64_t c1 = stop();
    return static_cast<double>(c1 - c0) / static_cast<double>(mono_ns() - t0);
}

// Median cost in ticks of an empty start()/stop() pair, subtracted from measurements. Counted
// in bins below 4096 ticks (the pair costs tens), so the median is exact without sorting.
inline std::uint64_t overhead(int reps = 10001) {
    static std::uint32_t bins[4097];
    for (auto& b : bins) b = 0;
    for (int i = 0; i < reps; ++i) {
        const std::uint64_t a = start();
        const std::uint64_t d = stop() - a;
        ++bins[d < 4096 ? d : 4096];
    }
    std::uint64_t seen = 0;
    for (std::uint64_t v = 0; v < 4097; ++v)
        if ((seen += bins[v]) > static_cast<std::uint64_t>(reps / 2)) return v;
    return 4096;
}

}  // namespace hft::tsc
