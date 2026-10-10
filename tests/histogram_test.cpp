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

TEST(Histogram, OneScanGivesTheSamePercentilesAndResetClearsEverything) {
    std::mt19937_64 rng(11);
    Histogram h;
    const double ps[] = {1, 50, 90, 99, 99.9, 100};
    for (int round = 0; round < 50; ++round) {
        std::lognormal_distribution<double> d(3.0 + round % 7, 0.5 + round % 3);
        const int n = round % 10 == 0 ? 0 : 1 + static_cast<int>(rng() % 5000);
        for (int i = 0; i < n; ++i) h.record(static_cast<std::uint64_t>(d(rng)));
        std::uint64_t got[6];
        h.percentiles(ps, got);
        for (int k = 0; k < 6; ++k) ASSERT_EQ(got[k], h.percentile(ps[k])) << round << " " << ps[k];
        h.reset();
        h.record(1ull << 60);  // above every earlier value: a bucket left over would come first
        ASSERT_EQ(h.percentile(1), 1ull << 60);
        h.reset();
    }
}

TEST(Histogram, MergeEqualsRecordingEverythingInOne) {
    std::mt19937_64 rng(3);
    std::lognormal_distribution<double> d(6.0, 2.0);
    Histogram all, sum, part;
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 1000 * (round % 4); ++i) {
            const auto v = static_cast<std::uint64_t>(d(rng));
            all.record(v), part.record(v);
        }
        sum.merge(part);
        part.reset();
    }
    EXPECT_EQ(sum.count(), all.count());
    EXPECT_EQ(sum.max(), all.max());
    EXPECT_EQ(sum.min(), all.min());
    EXPECT_EQ(sum.mean(), all.mean());
    for (const double p : {0.1, 1.0, 50.0, 99.0, 99.9, 99.99}) EXPECT_EQ(sum.percentile(p), all.percentile(p)) << p;
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
