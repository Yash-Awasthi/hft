#include <gtest/gtest.h>

#include "core/perf_counters.hpp"
#include "core/tsc.hpp"

TEST(Tsc, CalibratesAndOrders) {
    const double r = hft::tsc::ticks_per_ns(20);
    EXPECT_GT(r, 0.3);
    EXPECT_LT(r, 10.0);
    const std::uint64_t a = hft::tsc::start();
    const std::uint64_t b = hft::tsc::stop();
    EXPECT_GE(b, a);
    EXPECT_LT(hft::tsc::overhead(1001), 10'000u);
}

TEST(PerfCounters, CountsInstructionsWhenAvailable) {
    hft::PerfCounters pc;
    if (!pc.available(hft::PerfCounters::Instructions)) GTEST_SKIP() << "no PMU in this machine";
    volatile std::uint64_t x = 0;
    pc.start();
    for (int i = 0; i < 1'000'000; ++i) x = x + 1;
    pc.stop();
    EXPECT_GT(pc.total(hft::PerfCounters::Instructions), 1'000'000u);
}
