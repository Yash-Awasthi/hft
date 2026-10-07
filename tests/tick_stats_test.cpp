#include "strategy/tick_stats.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace hft::strategy;

TEST(TickStats, EtaCountsContinuationsAndAlternations) {
    EtaCounter e;
    // Changes: +, +, -, -, + -> pairs: cont, alt, cont, alt; repeats ignored.
    for (double m : {10.0, 10.0, 10.01, 10.02, 10.02, 10.01, 10.0, 10.01}) e.on_mid(m);
    EXPECT_EQ(e.continuations(), 2u);
    EXPECT_EQ(e.alternations(), 2u);
    EXPECT_DOUBLE_EQ(e.eta(), 0.5);
    EtaCounter bounce;
    for (int i = 0; i < 11; ++i) bounce.on_mid(i % 2 ? 10.01 : 10.0);
    EXPECT_DOUBLE_EQ(bounce.eta(), 0.0);
    EXPECT_TRUE(std::isnan(EtaCounter{}.eta()));
}

TEST(TickStats, ClockVarianceSamplesTheHoldingMid) {
    ClockVariance v(0, 10);
    v.advance(0, 1.0);    // grid 0: 1.0
    v.advance(25, 1.0);   // grids 10, 20 hold 1.0
    v.advance(31, 3.0);   // grid 30 held 3.0
    v.advance(40, 2.0);   // grid 40 holds 2.0
    EXPECT_EQ(v.samples(), 4u);
    EXPECT_DOUBLE_EQ(v.variance(), 4.0 + 1.0);
}
