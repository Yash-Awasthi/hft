#include "core/histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using hft::Histogram;

TEST(Histogram, BucketsTileTheRange) {
    for (std::size_t i = 1; i < (64 - Histogram::S + 2) * Histogram::kHalf; ++i) {
        ASSERT_EQ(Histogram::index(Histogram::lowest(i)), i) << i;
        ASSERT_EQ(Histogram::index(Histogram::lowest(i) - 1), i - 1) << i;
    }
    EXPECT_EQ(Histogram::index(~0ull), (64 - Histogram::S + 2) * Histogram::kHalf - 1);
}

TEST(Histogram, PercentilesWithinRelativeErrorOfExact) {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> d(5.0, 1.5);
    std::vector<std::uint64_t> v(200'000);
    Histogram h;
    for (auto& x : v) h.record(x = static_cast<std::uint64_t>(d(rng)));
    std::sort(v.begin(), v.end());
    for (const double p : {1.0, 50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
        const auto rank = static_cast<std::size_t>(std::max(1.0, std::floor(p / 100.0 * v.size() + 0.5)));
        const double exact = static_cast<double>(v[rank - 1]);
        EXPECT_NEAR(static_cast<double>(h.percentile(p)), exact, exact * 2.0 / Histogram::kHalf + 1) << p;
    }
    EXPECT_EQ(h.max(), v.back());
    EXPECT_EQ(h.min(), v.front());
    EXPECT_EQ(h.count(), v.size());
}

TEST(Histogram, SmallValuesAreExact) {
    Histogram h;
    for (std::uint64_t i = 1; i <= 1000; ++i) h.record(i);
    EXPECT_EQ(h.percentile(50), 500u);
    EXPECT_EQ(h.percentile(99), 990u);
    EXPECT_EQ(h.percentile(100), 1000u);
    h.reset();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(50), 0u);
}
