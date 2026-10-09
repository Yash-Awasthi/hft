// Moved out of tests/features_test.cpp.
#include "strategy/lifecycles.hpp"

TEST(Lifecycles, OutcomesQueueAndMarkouts) {
    LifecycleTracker t(1, 60.0, {0.001, 1.0});
    ItchWriter m;
    std::uint64_t seq = 0;
    auto on = [&](const ItchWriter& w) { t.step(w.b.data(), w.b.size(), ++seq); };
    const std::uint64_t t0 = m.ts;
    on(m.add(1, 'B', 100, 10'0000));
    on(m.add(2, 'S', 100, 10'0100));
    m.ts = t0 + 1'000'000;
    on(m.exec(1, 40));                // first fill of 1 after 1 ms
    on(m.add(3, 'B', 100, 10'0000));  // joins behind 60 shares
    on(m.add(5, 'S', 100, 10'0100));  // joins behind 100 shares
    m.ts = t0 + 3'000'000;
    on(m.add(6, 'S', 100, 10'0050));  // better ask: 2 and 5 move away
    m.ts = t0 + 4'000'000;
    on(m.del(3));  // own cancel: censored
    m.ts = t0 + 2'000'000'000;
    on(m.add(7, 'B', 1, 9'0000));  // a later event so markouts resolve
    t.finish();

    std::map<std::uint64_t, Lifecycle> by;
    for (const auto& l : t.lifecycles()) by[l.ref] = l;
    ASSERT_EQ(by.count(1) + by.count(2) + by.count(3) + by.count(5) + by.count(6), 5u);
    EXPECT_EQ(by[1].outcome, Outcome::Fill);
    EXPECT_NEAR(by[1].duration_s, 0.001, 1e-12);
    EXPECT_EQ(by[2].outcome, Outcome::Away);
    EXPECT_NEAR(by[2].duration_s, 0.003, 1e-12);
    EXPECT_EQ(by[5].outcome, Outcome::Away);
    EXPECT_DOUBLE_EQ(by[5].queue_ahead, 100);
    EXPECT_EQ(by[3].outcome, Outcome::Censored);
    EXPECT_DOUBLE_EQ(by[3].queue_ahead, 60);
    EXPECT_NEAR(by[3].duration_s, 0.003, 1e-12);
    EXPECT_FALSE(by.count(7));  // not at the best
    ASSERT_EQ(t.fills().size(), 1u);
    const FillMark& f = t.fills()[0];
    EXPECT_EQ(f.side, 1);
    // Mid 1000.5 ticks after the fill, 1000.25 a second later (ask improved to 10.005).
    EXPECT_DOUBLE_EQ(f.markout[0], 1000.5 - 1000);
    EXPECT_DOUBLE_EQ(f.markout[1], 1000.25 - 1000);
}
