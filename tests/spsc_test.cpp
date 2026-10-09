#include "core/spsc.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

using hft::SpscRing;

TEST(Spsc, FullAndEmptyAtCapacity) {
    SpscRing<int, 4> r;
    int v;
    EXPECT_FALSE(r.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(9));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(r.try_pop(v));
}

TEST(Spsc, TransfersInOrderAcrossThreads) {
    static SpscRing<std::uint64_t, 64> r;
    constexpr std::uint64_t n = 2'000'000;
    std::thread producer([] {
        for (std::uint64_t i = 1; i <= n; ++i) r.push(i);
    });
    std::uint64_t expect = 1, sum = 0;
    for (std::uint64_t i = 1; i <= n; ++i) {
        const std::uint64_t v = r.pop();
        if (v != expect) break;
        ++expect, sum += v;
    }
    producer.join();
    EXPECT_EQ(expect, n + 1);
    EXPECT_EQ(sum, n * (n + 1) / 2);
}
