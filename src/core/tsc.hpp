#pragma once

// Cycle timing with rdtscp. start() fences so earlier work is retired before the read;
// stop() uses rdtscp, which waits for earlier instructions, then fences so later work does
// not begin before the read.

#include <cpuid.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace hft::tsc {

inline std::uint64_t start() {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline std::uint64_t stop() {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// CPUID 0x80000007 EDX bit 8: the TSC rate does not change with frequency or C-states.
inline bool invariant() {
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000007, &a, &b, &c, &d)) return false;
    return (d >> 8) & 1;
}

// TSC ticks per nanosecond, measured against steady_clock over `ms` milliseconds.
inline double ticks_per_ns(int ms = 100) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = start();
    while (clock::now() - t0 < std::chrono::milliseconds(ms)) {
    }
    const std::uint64_t c1 = stop();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
    return static_cast<double>(c1 - c0) / static_cast<double>(ns);
}

// Median cost in ticks of an empty start()/stop() pair, subtracted from measurements.
inline std::uint64_t overhead(int reps = 10001) {
    std::vector<std::uint64_t> v(static_cast<std::size_t>(reps));
    for (auto& x : v) {
        const std::uint64_t a = start();
        x = stop() - a;
    }
    std::nth_element(v.begin(), v.begin() + reps / 2, v.end());
    return v[static_cast<std::size_t>(reps / 2)];
}

}  // namespace hft::tsc
