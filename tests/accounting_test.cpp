#include "backtest/accounting.hpp"

#include <gtest/gtest.h>

#include "core/philox.hpp"

using namespace hft::backtest;

TEST(Accounting, KnownAnswerRoundTrip) {
    Accounting a;
    a.mid(10'0000, 10'0100, 0);  // mid $10.005
    // Buy 100 at the bid $10.00 as maker, rebate $0.0020 per share.
    a.fill(+1, 10'0000, 100, -200'000, 1'000);
    EXPECT_EQ(a.inventory(), 100);
    EXPECT_EQ(a.spread_capture(), 500'000);    // $0.005 below the mid on 100 shares
    a.mid(10'0100, 10'0200, 2'000);            // mid up one tick
    a.fill(-1, 10'0200, 100, 300'000, 3'000);  // sell at the ask as taker, fee $0.0030
    EXPECT_EQ(a.inventory(), 0);
    EXPECT_TRUE(a.identity_holds());
    // Cash: -1000 + 1002 dollars of round trip = +$2.00, plus $0.20 rebate, less $0.30 fee.
    EXPECT_EQ(a.total_pnl(), 2'000'000 + 200'000 - 300'000);
    EXPECT_EQ(a.fee_pnl(), 200'000 - 300'000);
    EXPECT_EQ(a.inventory_pnl(), 100 * 100 * 100);  // 100 shares x $0.01 move
}

TEST(Accounting, AdverseSelectionResolvesOneSecondAfterEachFill) {
    Accounting a;
    a.mid(10'0000, 10'0100, 0);
    a.fill(+1, 10'0000, 100, 0, 0);
    a.mid(9'9900, 10'0000, 500'000'000);   // mid down a tick within the second
    a.mid(9'9800, 9'9900, 1'500'000'000);  // after the second: not counted
    a.flush();
    EXPECT_EQ(a.adverse_selection(), -100 * 100 * 100);
    EXPECT_TRUE(a.identity_holds());
}

TEST(Accounting, IdentityHoldsOnRandomFillsAndMoves) {
    const hft::rng::Stream g(9, 0, 0);
    Accounting a;
    std::uint32_t bid = 50'0000;
    for (std::uint32_t i = 0; i < 20000; ++i) {
        const auto d = g.draw(i, 0);
        if (d[0] % 3 == 0) {
            bid = bid + 100 * (d[1] % 3) - 100;
            a.mid(bid, bid + 100 * (1 + d[2] % 3), i * 1'000'000ull);
        } else {
            const int side = d[1] % 2 ? 1 : -1;
            a.fill(side, bid + 100 * (d[2] % 4), 1 + d[3] % 500,
                   static_cast<std::int64_t>(d[3] % 7000) - 2000, i * 1'000'000ull);
        }
        ASSERT_TRUE(a.identity_holds()) << i;
    }
}
