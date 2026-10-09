// Moved out of tests/backtest_test.cpp.
#include "strategy/grid.hpp"

TEST(Grid, StepsCoverTheWindowAndCountExecutionsAtTheBest) {
    FixtureStore fx;
    const Config w = window();
    strategy::GridSampler g(100'000'000, w.start_ns, w.start_ns + 15'000'000'000ull);
    g.run(fx.dir.path.string(), 1, {3});
    const auto& s = g.steps();
    ASSERT_GT(s.size(), 100u);
    EXPECT_LE(s.size(), 150u);
    EXPECT_EQ(g.rows().size(), s.size() * g.features());
    double exec = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) {
            EXPECT_EQ(s[i].ts, s[i - 1].ts + 100'000'000);
        }
        EXPECT_GT(s[i].ask_px, s[i].bid_px);
        EXPECT_TRUE(std::isfinite(s[i].mid_end));
        for (int k = 0; k < 2; ++k) {
            EXPECT_GE(s[i].exec[k], 0);
            EXPECT_GE(s[i].cancel[k], 0);
            exec += s[i].exec[k];
        }
        // A side that held its price ends the step at the same best.
        if (i + 1 < s.size() && !s[i].moved[0]) {
            EXPECT_EQ(s[i + 1].bid_px, s[i].bid_px);
        }
    }
    EXPECT_GT(exec, 0);
}
