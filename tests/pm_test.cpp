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
