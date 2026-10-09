#include "strategy/trades.hpp"

#include <gtest/gtest.h>

#include <cstring>

#include "itch_writer.hpp"

using namespace hft;

TEST(Trades, SweepsAreOneSignedTradeWithMidsAndAttribution) {
    strategy::TradeRecorder t(0, ~0ull);
    ItchWriter m;
    auto on = [&](const ItchWriter& w) { t.on_itch(w.b.data(), w.b.size()); };
    on(m.add(1, 'B', 100, 10'0000));
    on(m.addf(2, 'S', 100, 10'0100, "ABCD"));
    on(m.add(3, 'S', 200, 10'0200));
    m.ts += 1000;
    on(m.exec(2, 100));  // a buy sweeps the 10.01 level and part of 10.02
    on(m.exec(3, 50));
    m.ts += 1000;
    on(m.exec(1, 40));   // seller-initiated
    on(m.hidden('B', 30, 10'0150));  // above the mid (10.01): buyer-initiated whatever the side field
    m.ts += 1000;
    on(m.hidden('B', 20, 10'0100));  // at the mid: unsigned, dropped
    m.ts += 1000;
    on(m.hidden('B', 10, 10'0050));  // below the mid: seller-initiated
    t.close();
    const auto& r = t.record();
    ASSERT_EQ(r.ts.size(), 4u);
    EXPECT_EQ(r.sign[3], -1);
    EXPECT_EQ(r.shares[3], 10u);
    EXPECT_EQ(r.sign[0], 1);
    EXPECT_EQ(r.shares[0], 150u);
    EXPECT_DOUBLE_EQ(r.mid_before[0], 10.005);
    EXPECT_DOUBLE_EQ(r.mid_after[0], 10.01);
    EXPECT_NEAR(r.notional[0], 100 * 10.01 + 50 * 10.02, 1e-9);
    EXPECT_EQ(r.sign[1], -1);
    EXPECT_EQ(r.shares[1], 40u);
    EXPECT_EQ(r.sign[2], 1);
    EXPECT_EQ(r.shares[2], 30u);
    ASSERT_EQ(r.exec_ts.size(), 5u);
    std::uint32_t abcd;
    std::memcpy(&abcd, "ABCD", 4);
    EXPECT_EQ(r.exec_mpid[0], abcd);
    EXPECT_EQ(r.exec_mpid[1], 0u);
    EXPECT_EQ(r.exec_hidden[3], 1);
}
