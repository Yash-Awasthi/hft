#include <gtest/gtest.h>

#include <vector>

#include "book/btree_book.hpp"
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

using Books = ::testing::Types<MapBook, TickBook<LinearMap>, TickBook<RobinHoodMap>,
                               TickBook<DirectMap<>>, TickBook<LinearMap, Aos>,
                               TickBook<LinearMap, Soa>, SortedVecBook<>, BTreeBook<>>;
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

// A loaded snapshot replayed forward must serialise byte-identically to the original.
template <class B>
class TickSnapshot : public ::testing::Test {};
using TickBooks =
    ::testing::Types<TickBook<LinearMap>, TickBook<RobinHoodMap>, TickBook<DirectMap<16>>,
                     TickBook<LinearMap, Aos>, TickBook<LinearMap, Soa>>;
TYPED_TEST_SUITE(TickSnapshot, TickBooks);

TYPED_TEST(TickSnapshot, LoadThenReplayMatches) {
    TypeParam a(16);
    std::uint64_t ref = 1;
    auto step = [&](TypeParam& b, std::uint64_t i) {
        const std::uint32_t px = 20'0000 + static_cast<std::uint32_t>(i * 7919 % 300) * 100;
        b.add(ref + i, i % 2 ? kS : kB, 100, i % 11 == 0 ? px + 3 : px, i);
        if (i % 3 == 0) b.erase(ref + i / 2);
        if (i % 5 == 0) b.execute(ref + i - 1, 10);
    };
    for (std::uint64_t i = 0; i < 2000; ++i) step(a, i);
    std::vector<std::uint8_t> snap;
    a.save(snap);

    TypeParam b;
    ASSERT_TRUE(b.load(snap.data(), snap.size()));
    EXPECT_TRUE(b.check());
    EXPECT_EQ(b.bbo(), a.bbo());
    for (std::uint64_t i = 2000; i < 4000; ++i) {
        step(a, i);
        step(b, i);
    }
    std::vector<std::uint8_t> sa, sb;
    a.save(sa);
    b.save(sb);
    EXPECT_EQ(sa, sb);
    snap[0] ^= 1;
    EXPECT_FALSE(b.load(snap.data(), snap.size()));
    EXPECT_FALSE(b.load(sa.data(), sa.size() - 1));
}

// A fork replayed forward must match the original replayed forward, byte for byte, and the
// two must not share state.
TYPED_TEST(TickSnapshot, ForkThenReplayMatches) {
    TypeParam a(16);
    auto step = [](TypeParam& b, std::uint64_t i) {
        const std::uint32_t px = 30'0000 + static_cast<std::uint32_t>(i * 7919 % 4000) * 100;
        b.add(1 + i, i % 2 ? Side::Sell : Side::Buy, 100, i % 13 == 0 ? px + 7 : px, i);
        if (i % 3 == 0) b.erase(1 + i / 2);
        if (i % 5 == 0) b.execute(i, 10);
    };
    for (std::uint64_t i = 0; i < 3000; ++i) step(a, i);
    TypeParam b;
    b.copy_from(a);
    EXPECT_TRUE(b.check());
    for (std::uint64_t i = 3000; i < 6000; ++i) {
        step(a, i);
        step(b, i);
    }
    std::vector<std::uint8_t> sa, sb;
    a.save(sa);
    b.save(sb);
    EXPECT_EQ(sa, sb);
    b.erase(5999);
    EXPECT_NE(a.order_count(), b.order_count());
}

TYPED_TEST(BookTest, RejectsReferencesBeyondPackedRange) {
    EXPECT_FALSE(this->b.add(kMaxRef, kB, 1, 10'0000, 1));
    EXPECT_TRUE(this->b.add(kMaxRef - 1, kB, 1, 10'0000, 1));
    EXPECT_FALSE(this->b.replace(kMaxRef - 1, kMaxRef, 1, 10'0000, 2));
    EXPECT_EQ(this->b.shares(kMaxRef - 1), 1u);
}

template <class B>
class TickQueue : public ::testing::Test {};
TYPED_TEST_SUITE(TickQueue, TickBooks);

// Queue walk used by matching: best order first, then the orders behind it, with owners.
TYPED_TEST(TickQueue, FrontAndBehindWalkTimePriority) {
    TypeParam b;
    EXPECT_FALSE(b.front(kS).has_value());
    b.add(1, kS, 100, 10'0100, 1, 7);
    b.add(2, kS, 200, 10'0000, 2, 8);
    b.add(3, kS, 300, 10'0000, 3, 0);
    b.add(4, kB, 50, 9'9900, 4, 9);
    auto f = b.front(kS);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->ref, 2u);
    EXPECT_EQ(f->price, 10'0000u);
    EXPECT_EQ(f->qty, 200u);
    EXPECT_EQ(f->owner, 8u);
    EXPECT_EQ(f->seq, 2u);
    auto n = b.behind(2);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->ref, 3u);
    EXPECT_EQ(n->owner, 0u);
    EXPECT_FALSE(b.behind(3).has_value());
    EXPECT_EQ(b.front(kB)->owner, 9u);
    b.erase(2);
    b.erase(3);
    EXPECT_EQ(b.front(kS)->ref, 1u);
    EXPECT_EQ(b.order(1)->owner, 7u);
    EXPECT_FALSE(b.order(2).has_value());
}

// Half-penny grid: $0.005 ticks land in the window, and a level far away in the radix.
TEST(TickBookTick, HalfPennyGrid) {
    TickBook<> b(1024, 50);
    ASSERT_TRUE(b.add(1, kB, 100, 10'0050, 1));
    ASSERT_TRUE(b.add(2, kS, 100, 10'0100, 2));
    ASSERT_TRUE(b.add(3, kB, 100, 1'0050, 3));
    ASSERT_TRUE(b.add(4, kB, 100, 10'0030, 4));  // off the half-penny grid
    EXPECT_EQ(b.bbo(), (Bbo{10'0050, 10'0100, 100, 100}));
    EXPECT_EQ(b.overflow_levels(), 2u);
    EXPECT_TRUE(b.check());
    b.erase(1);
    EXPECT_EQ(b.bbo().bid_px, 10'0030u);
    b.clear();
    ASSERT_TRUE(b.add(5, kB, 100, 3'0050, 5));
    EXPECT_EQ(b.overflow_levels(), 0u);
}
