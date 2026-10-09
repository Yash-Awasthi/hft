#include "pm/book.hpp"

#include <gtest/gtest.h>

using namespace hft::pm;

TEST(PmBook, ParsesPrices) {
    EXPECT_EQ(parse_price("0.366"), 3660);
    EXPECT_EQ(parse_price("0.5"), 5000);
    EXPECT_EQ(parse_price("1"), 10000);
    EXPECT_EQ(parse_price("0.0001"), 1);
    EXPECT_EQ(parse_price("0.123456"), 1234);  // finer than the grid is cut, not rounded
    for (const char* bad : {"", ".", "x", "0.1x", "2.0", "-0.1"}) EXPECT_EQ(parse_price(bad), -1) << bad;
}

TEST(PmBook, AppliesLevelsAndRemovesAtZero) {
    TokenBook b;
    EXPECT_FALSE(b.two_sided());
    b.set(true, 3600, 100);
    b.set(true, 3590, 50);
    b.set(false, 3700, 80);
    ASSERT_TRUE(b.two_sided());
    EXPECT_EQ(b.best_bid(), 3600);
    EXPECT_EQ(b.best_ask(), 3700);
    EXPECT_FALSE(b.crossed());
    b.set(true, 3600, 0);
    EXPECT_EQ(b.best_bid(), 3590);
    b.set(true, 3700, 10);  // a bid at the ask crosses the book
    EXPECT_TRUE(b.crossed());
    b.clear();
    EXPECT_EQ(b.levels(), 0u);
}

TEST(PmBook, DepthWithinWidthOfBest) {
    TokenBook b;
    b.set(true, 5000, 10);
    b.set(true, 4900, 20);
    b.set(true, 4000, 1000);  // too far from the best
    b.set(false, 5100, 5);
    EXPECT_DOUBLE_EQ(b.depth_bid(500), 30.0);
    EXPECT_DOUBLE_EQ(b.depth_ask(500), 5.0);
}

#include "pm/maker.hpp"

namespace {

hft::pm::MakerParams params() {
    hft::pm::MakerParams p;
    p.warmup_s = 0;
    p.requote_s = 0;
    return p;
}

// Bids 0.49 (500 shares) and 0.48, asks 0.51 (500) and 0.52.
void seed(hft::pm::TokenMaker& m) {
    m.book.set(true, 4900, 500);
    m.book.set(true, 4800, 500);
    m.book.set(false, 5100, 500);
    m.book.set(false, 5200, 500);
    m.on_snapshot(1'000'000'000);
}

}  // namespace

TEST(PmMaker, QuotesStayInsideTheSpreadAndNeverCross) {
    hft::pm::TokenMaker m(params());
    seed(m);
    ASSERT_GE(m.bid_px(), 0);
    ASSERT_GE(m.ask_px(), 0);
    EXPECT_LT(m.bid_px(), m.ask_px());
    EXPECT_LE(m.bid_px(), 5100 - 100);
    EXPECT_GE(m.ask_px(), 4900 + 100);
}

TEST(PmMaker, TradeThroughFillsTheWholeOrder) {
    hft::pm::TokenMaker m(params());
    seed(m);
    const std::int32_t b = m.bid_px();
    m.on_trade(2'000'000'000, /*taker_buy=*/false, b - 100, 1);
    EXPECT_EQ(m.buys(), 1u);
    EXPECT_DOUBLE_EQ(m.inventory(), 100.0);
    EXPECT_NEAR(m.pnl(), 100 * (0.5 - b * 1e-4), 1e-9);  // bought below the mid
}

TEST(PmMaker, TradeAtOurPriceFillsOnlyAfterTheQueueAhead) {
    auto p = params();
    p.k = 1e6;  // negligible base spread, so the bid joins the best
    hft::pm::TokenMaker m(p);
    seed(m);
    ASSERT_EQ(m.bid_px(), 4900);  // joins the best bid, behind its 500 shares
    m.on_trade(2'000'000'000, false, 4900, 300);
    EXPECT_EQ(m.buys(), 0u);
    m.on_trade(3'000'000'000, false, 4900, 300);  // 200 left ahead, so 100 reach us
    EXPECT_EQ(m.buys(), 1u);
    EXPECT_DOUBLE_EQ(m.inventory(), 100.0);
}

TEST(PmMaker, SnapshotCancelsQuotesAndWarmupDelaysThem) {
    auto p = params();
    p.warmup_s = 10;
    hft::pm::TokenMaker m(p);
    seed(m);
    EXPECT_LT(m.bid_px(), 0);  // still warming up
    m.on_trade(20'000'000'000, true, 5100, 1);
    EXPECT_GE(m.bid_px(), 0);
    m.on_snapshot(20'000'000'001);
    EXPECT_GE(m.bid_px(), 0);  // cancelled, then placed again by the same update
}
