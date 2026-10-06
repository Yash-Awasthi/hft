#include <gtest/gtest.h>

#include "book/map_book.hpp"
#include "book/tick_book.hpp"

using namespace hft::book;

namespace {

template <class B>
class BookTest : public ::testing::Test {
   protected:
    B b;
    void TearDown() override { EXPECT_TRUE(b.check()); }
};

using Books = ::testing::Types<MapBook, TickBook<LinearMap>, TickBook<RobinHoodMap>>;
TYPED_TEST_SUITE(BookTest, Books);

constexpr Side kB = Side::Buy;
constexpr Side kS = Side::Sell;

}  // namespace

TYPED_TEST(BookTest, EmptyBook) {
    EXPECT_EQ(this->b.bbo(), Bbo{});
    EXPECT_EQ(this->b.order_count(), 0u);
}

TYPED_TEST(BookTest, BestLevelsAggregate) {
    auto& b = this->b;
    ASSERT_TRUE(b.add(1, kB, 100, 100'0000, 1));
    ASSERT_TRUE(b.add(2, kB, 200, 100'0100, 2));
    ASSERT_TRUE(b.add(3, kB, 50, 100'0100, 3));
    ASSERT_TRUE(b.add(4, kS, 300, 100'0300, 4));
    ASSERT_TRUE(b.add(5, kS, 10, 100'0200, 5));
    EXPECT_EQ(b.bbo(), (Bbo{100'0100, 100'0200, 250, 10}));
    EXPECT_EQ(b.order_count(), 5u);
}

TYPED_TEST(BookTest, ExecuteCancelDeleteWalkTheBook) {
    auto& b = this->b;
    b.add(1, kS, 100, 50'0000, 1);
    b.add(2, kS, 100, 50'0000, 2);
    b.add(3, kS, 70, 50'0100, 3);
    ASSERT_TRUE(b.execute(1, 40));
    EXPECT_EQ(b.bbo(), (Bbo{0, 50'0000, 0, 160}));
    ASSERT_TRUE(b.execute(1, 60));
    EXPECT_EQ(b.bbo(), (Bbo{0, 50'0000, 0, 100}));
    ASSERT_TRUE(b.cancel(2, 99));
    EXPECT_EQ(b.bbo(), (Bbo{0, 50'0000, 0, 1}));
    ASSERT_TRUE(b.erase(2));
    EXPECT_EQ(b.bbo(), (Bbo{0, 50'0100, 0, 70}));
    ASSERT_TRUE(b.cancel(3, 70));
    EXPECT_EQ(b.bbo(), Bbo{});
    EXPECT_EQ(b.order_count(), 0u);
}

TYPED_TEST(BookTest, ReplaceLosesPriorityAndKeepsSide) {
    auto& b = this->b;
    b.add(1, kB, 100, 10'0000, 1);
    b.add(2, kB, 200, 10'0000, 2);
    EXPECT_EQ(b.queue_ahead(1), 0);
    EXPECT_EQ(b.queue_ahead(2), 100);
    ASSERT_TRUE(b.replace(1, 9, 100, 10'0000, 3));
    EXPECT_EQ(b.queue_ahead(9), 200);
    EXPECT_EQ(b.queue_ahead(2), 0);
    EXPECT_EQ(b.queue_ahead(1), -1);
    ASSERT_TRUE(b.replace(9, 10, 5, 10'0500, 4));
    EXPECT_EQ(b.bbo(), (Bbo{10'0500, 0, 5, 0}));
}

TYPED_TEST(BookTest, RejectsInvalidAndLeavesBookUnchanged) {
    auto& b = this->b;
    b.add(1, kB, 100, 10'0000, 1);
    b.add(2, kS, 100, 11'0000, 2);
    const Bbo before = b.bbo();
    EXPECT_FALSE(b.add(1, kS, 5, 12'0000, 3));
    EXPECT_FALSE(b.add(3, kS, 0, 12'0000, 3));
    EXPECT_FALSE(b.execute(7, 1));
    EXPECT_FALSE(b.execute(1, 101));
    EXPECT_FALSE(b.cancel(1, 0));
    EXPECT_FALSE(b.erase(7));
    EXPECT_FALSE(b.replace(7, 8, 1, 10'0000, 4));
    EXPECT_FALSE(b.replace(1, 2, 1, 10'0000, 4));
    EXPECT_FALSE(b.replace(1, 8, 0, 10'0000, 4));
    EXPECT_EQ(b.bbo(), before);
    EXPECT_EQ(b.order_count(), 2u);
}

TYPED_TEST(BookTest, FarPricesAndExtremes) {
    auto& b = this->b;
    ASSERT_TRUE(b.add(1, kB, 1, 1, 1));            // $0.0001 stub bid
    ASSERT_TRUE(b.add(2, kS, 1, 199999'9900, 2));  // $199,999.99 stub ask
    ASSERT_TRUE(b.add(3, kB, 100, 25'0000, 3));
    ASSERT_TRUE(b.add(4, kS, 100, 25'0100, 4));
    EXPECT_EQ(b.bbo(), (Bbo{25'0000, 25'0100, 100, 100}));
    b.erase(3);
    b.erase(4);
    EXPECT_EQ(b.bbo(), (Bbo{1, 199999'9900, 1, 1}));
    ASSERT_TRUE(b.add(5, kB, 7, 25'0050, 5));  // off the penny grid
    EXPECT_EQ(b.bbo(), (Bbo{25'0050, 199999'9900, 7, 1}));
}

TYPED_TEST(BookTest, RejectsPricesOutsideRange) {
    EXPECT_FALSE(this->b.add(1, kB, 1, 0, 1));
    EXPECT_FALSE(this->b.add(1, kB, 1, 1u << 31, 1));
    EXPECT_TRUE(this->b.add(1, kB, 1, (1u << 31) - 1, 1));
}

// Drives the price far beyond one window in both directions so levels cross between the
// window and overflow, with resting orders left behind at every step.
TYPED_TEST(BookTest, DriftAcrossManyWindows) {
    auto& b = this->b;
    std::uint64_t ref = 1;
    for (std::uint32_t step = 0; step < 200; ++step) {
        const std::uint32_t mid = 100'0000 + step * 50'0000 / 10;
        ASSERT_TRUE(b.add(ref, kB, 100, mid - 100, ref));
        ++ref;
        ASSERT_TRUE(b.add(ref, kS, 100, mid + 100, ref));
        ++ref;
        if (step % 3 == 0) {
            ASSERT_TRUE(b.add(ref, kB, 5, mid - 7, ref));
            ++ref;
        }
    }
    EXPECT_TRUE(b.check());
    EXPECT_EQ(b.bbo().bid_px, 100'0000 + 199 * 5'0000 - 100);
    EXPECT_EQ(b.bbo().ask_px, 100'0000 + 100);
    for (std::uint64_t r = ref - 1; r >= 1; --r) b.erase(r);
    EXPECT_EQ(b.bbo(), Bbo{});
}

TYPED_TEST(BookTest, SubDollarPrices) {
    auto& b = this->b;
    ASSERT_TRUE(b.add(1, kB, 1000, 3412, 1));  // $0.3412
    ASSERT_TRUE(b.add(2, kS, 1000, 3415, 2));
    ASSERT_TRUE(b.add(3, kS, 50, 1'0100, 3));  // $1.01
    EXPECT_EQ(b.bbo(), (Bbo{3412, 3415, 1000, 1000}));
    b.erase(2);
    EXPECT_EQ(b.bbo(), (Bbo{3412, 1'0100, 1000, 50}));
}

TYPED_TEST(BookTest, SharesAndRestingTotal) {
    auto& b = this->b;
    b.add(1, kB, 100, 10'0000, 1);
    b.add(2, kS, 40, 10'0100, 2);
    b.add(3, kS, 7, 10'0150, 3);  // overflow level in the tick book
    b.execute(1, 30);
    EXPECT_EQ(b.shares(1), 70u);
    EXPECT_EQ(b.shares(9), 0u);
    EXPECT_EQ(b.resting_shares(), 117u);
    b.erase(2);
    EXPECT_EQ(b.resting_shares(), 77u);
}
