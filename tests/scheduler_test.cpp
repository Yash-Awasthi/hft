#include "engine/scheduler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

using namespace hft::engine;

TEST(Scheduler, OrdersByTimeThenKindThenInsertion) {
    EventQueue q(16);
    q.push(100, Kind::Report, 1);
    q.push(100, Kind::Market, 2);
    q.push(50, Kind::OrderArrival, 3);
    q.push(100, Kind::OrderArrival, 4);
    q.push(100, Kind::Market, 5);
    q.push(100, Kind::MarketData, 6);
    std::vector<std::uint32_t> got;
    Timed t;
    while (q.pop(t)) got.push_back(t.payload);
    // 50 first; at 100 market events (in insertion order), then market data, orders, reports.
    EXPECT_EQ(got, (std::vector<std::uint32_t>{3, 2, 5, 6, 4, 1}));
}

TEST(Scheduler, MatchesSortedReferenceOnRandomPushesAndPops) {
    std::mt19937_64 g(3);
    EventQueue q(4);
    std::vector<std::tuple<std::uint64_t, int, std::uint64_t, std::uint32_t>> ref;
    std::uint64_t ins = 0;
    for (int i = 0; i < 20000; ++i) {
        if (g() % 3 && q.size() < 10000) {
            const std::uint64_t t = g() % 1000;
            const Kind k = static_cast<Kind>(g() % 4);
            q.push(t, k, static_cast<std::uint32_t>(i));
            ref.emplace_back(t, static_cast<int>(k), ins++, static_cast<std::uint32_t>(i));
            continue;
        }
        Timed t;
        if (!q.pop(t)) {
            ASSERT_TRUE(ref.empty());
            continue;
        }
        const auto it = std::min_element(ref.begin(), ref.end());
        EXPECT_EQ(t.payload, std::get<3>(*it));
        ref.erase(it);
    }
}
