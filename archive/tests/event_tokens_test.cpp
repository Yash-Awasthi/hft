#include "strategy/event_tokens.hpp"

#include <gtest/gtest.h>

#include "itch_writer.hpp"

using namespace hft;

TEST(Tokens, TypesDistancesSizesAndTimes) {
    strategy::Tokenizer t(0, ~0ull);
    ItchWriter m;
    std::uint64_t seq = 0;
    auto on = [&](const ItchWriter& w) { t.on_itch(w.b.data(), w.b.size(), ++seq); };
    on(m.add(1, 'B', 100, 10'0000));
    on(m.add(2, 'S', 300, 10'0100));  // first two: no mid before, not recorded
    m.ts += 2000;
    on(m.add(3, 'B', 50, 9'9800));    // 2.5 ticks below the mid 1000.5
    m.ts += 1000;
    on(m.exec(2, 100));
    on(m.cancel(3, 10));
    on(m.hidden('B', 40, 10'0080));   // above the mid: buyer-initiated, side 1
    const auto& r = t.record();
    ASSERT_EQ(r.ts.size(), 4u);
    EXPECT_EQ(r.type[0], 0);
    EXPECT_EQ(r.side[0], 0);
    EXPECT_EQ(r.dist[0], 2);
    EXPECT_EQ(r.size[0], 0);
    EXPECT_FLOAT_EQ(r.log_dt[0], 0.0f);
    EXPECT_EQ(r.type[1], 1);
    EXPECT_EQ(r.side[1], 1);
    EXPECT_EQ(r.dist[1], 0);
    EXPECT_EQ(r.size[1], 1);
    EXPECT_FLOAT_EQ(r.log_dt[1], static_cast<float>(std::log1p(1.0)));
    EXPECT_EQ(r.type[2], 2);
    EXPECT_EQ(r.type[3], 4);
    EXPECT_EQ(r.side[3], 1);
    EXPECT_DOUBLE_EQ(r.mid_after[1], 1000.5);
}
