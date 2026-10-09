#include "pm/arb.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace hft::pm;

namespace {

struct Fixture {
    std::vector<TokenBook> books = std::vector<TokenBook>(3);
    ArbScanner s;
    std::vector<ArbWindow> closed;
    void touch(std::uint32_t t, std::int64_t ns) {
        s.on_book(t, ns, [&](std::uint32_t i) -> const TokenBook& { return books[i]; },
                  [&](const ArbWindow& w) { closed.push_back(w); });
    }
};

}  // namespace

TEST(Arb, OpensWhenAsksSumBelowOneAndClosesWhenTheyDoNot) {
    Fixture f;
    f.s.add_group("three-way", {0, 1, 2});
    f.books[0].set(false, 3000, 50);
    f.books[1].set(false, 3000, 20);
    f.touch(1, 1);
    EXPECT_EQ(f.s.open_windows(), 0u);  // one leg has no ask yet
    f.books[2].set(false, 3500, 80);
    f.touch(2, 10);  // 0.30 + 0.30 + 0.35 = 0.95
    EXPECT_EQ(f.s.open_windows(), 1u);
    f.books[2].set(false, 3500, 0);
    f.books[2].set(false, 3400, 5);
    f.touch(2, 20);  // edge grows to 0.06, thinnest leg 5 shares
    f.books[0].set(false, 3000, 0);
    f.books[0].set(false, 3700, 10);
    f.touch(0, 50);  // 1.01: closed
    ASSERT_EQ(f.closed.size(), 1u);
    const ArbWindow& w = f.closed[0];
    EXPECT_TRUE(w.buy);
    EXPECT_EQ(w.open_ns, 10);
    EXPECT_EQ(w.close_ns, 50);
    EXPECT_EQ(w.open_edge, 500);
    EXPECT_EQ(w.max_edge, 600);
    EXPECT_EQ(w.size_at_max, 5.0);
    EXPECT_EQ(f.s.stats()[0].windows, 1u);
    EXPECT_DOUBLE_EQ(f.s.stats()[0].open_seconds, 40e-9);
}

TEST(Arb, BidsAboveOneAndFinishClosesOpenWindows) {
    Fixture f;
    f.s.add_group("pair", {0, 1});
    f.books[0].set(true, 6000, 10);
    f.books[1].set(true, 4100, 30);
    f.touch(0, 5);
    EXPECT_EQ(f.s.open_windows(), 1u);
    f.s.finish(9, [&](const ArbWindow& w) { f.closed.push_back(w); });
    ASSERT_EQ(f.closed.size(), 1u);
    EXPECT_FALSE(f.closed[0].buy);
    EXPECT_EQ(f.closed[0].max_edge, 100);
    EXPECT_EQ(f.closed[0].size_at_max, 10.0);
    EXPECT_EQ(f.s.open_windows(), 0u);
}

TEST(Arb, TokensOutsideAnyGroupAreIgnored) {
    Fixture f;
    f.s.add_group("pair", {0, 1});
    f.books[2].set(false, 1, 1);
    f.touch(2, 1);
    EXPECT_EQ(f.s.stats()[0].checks, 0u);
}
