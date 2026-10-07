#include "engine/replay_exchange.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "itch_writer.hpp"

using namespace hft::engine;

namespace {

using Itch = ItchWriter;

struct Log {
    std::vector<Event> ev;
    void operator()(const Event& e) { ev.push_back(e); }
    std::uint32_t filled(std::uint64_t ref) const {
        std::uint32_t q = 0;
        for (const Event& e : ev)
            if (e.type == EventType::Fill && e.ref == ref) q += e.qty;
        return q;
    }
};

constexpr Side kB = Side::Buy;

void feed(ReplayExchange<>& x, const Itch& m, Log& l) { x.on_itch(m.b.data(), m.b.size(), l); }

}  // namespace

// A virtual bid joins behind two real bids. Executions of orders ahead of it do not fill it;
// an execution of a real order that joined behind it does, and is counted as diverted.
TEST(ReplayExchange, FillsOnlyWhenTheAggressorReachesOurQueuePosition) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    feed(x, m.add(1, 'B', 100, 10'0000), l);
    feed(x, m.add(2, 'B', 100, 10'0000), l);
    const auto v = x.submit({1, kB, 10'0000, 50}, l);
    feed(x, m.add(3, 'B', 100, 10'0000), l);
    feed(x, m.exec(1, 100), l);
    feed(x, m.exec(2, 60), l);
    EXPECT_EQ(l.filled(v), 0u);
    feed(x, m.exec(3, 30), l);
    EXPECT_EQ(l.filled(v), 30u);
    EXPECT_EQ(x.divergence().diverted, 30u);
    feed(x, m.exec(3, 70), l);
    EXPECT_EQ(l.filled(v), 50u);
    EXPECT_EQ(x.divergence().diverted, 50u);
    EXPECT_FALSE(x.book().order(v).has_value());
    EXPECT_TRUE(x.book().check());
}

TEST(ReplayExchange, TradeThroughFillsUnderBothRules) {
    for (FillRule r : {FillRule::Queue, FillRule::TradeThrough}) {
        ReplayExchange<> x({}, r);
        Log l;
        Itch m;
        feed(x, m.add(1, 'B', 100, 10'0000), l);
        const auto v = x.submit({1, kB, 10'0100, 40}, l);  // improves the bid
        feed(x, m.exec(1, 100), l);
        EXPECT_EQ(l.filled(v), 40u);
        EXPECT_EQ(x.divergence().through_fills, 1u);
    }
}

// A hidden buy executed at our displayed price: displayed orders rank first, so the queue
// rule fills us; the conservative rule needs a print beyond our price.
TEST(ReplayExchange, HiddenExecutionAtOurPrice) {
    for (FillRule r : {FillRule::Queue, FillRule::TradeThrough}) {
        ReplayExchange<> x({}, r);
        Log l;
        Itch m;
        const auto v = x.submit({1, kB, 10'0000, 40}, l);
        feed(x, m.hidden('B', 25, 10'0000), l);
        EXPECT_EQ(l.filled(v), r == FillRule::Queue ? 25u : 0u);
        feed(x, m.hidden('B', 25, 9'9950), l);  // midpoint below us
        EXPECT_EQ(l.filled(v), r == FillRule::Queue ? 40u : 25u);
    }
}

// Taking real liquidity does not change the replayed book, but the same shares cannot be
// taken twice until the real order leaves the book.
TEST(ReplayExchange, TakingConsumesRealSharesOnce) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    feed(x, m.add(1, 'S', 100, 10'0000), l);
    feed(x, m.add(2, 'S', 100, 10'0100), l);
    const auto a = x.submit({1, kB, 10'0100, 150, Tif::Ioc}, l);
    EXPECT_EQ(l.filled(a), 150u);
    const auto b = x.submit({1, kB, 10'0100, 100, Tif::Ioc}, l);
    EXPECT_EQ(l.filled(b), 50u);
    EXPECT_EQ(x.divergence().taken, 200u);
    EXPECT_EQ(x.book().bbo().ask_qty, 100u);  // the replayed book is unchanged
    feed(x, m.del(2), l);
    feed(x, m.add(3, 'S', 100, 10'0100), l);
    const auto c = x.submit({1, kB, 10'0100, 100, Tif::Ioc}, l);
    EXPECT_EQ(l.filled(c), 100u);
}

TEST(ReplayExchange, PostOnlyAndCancel) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    feed(x, m.add(1, 'S', 100, 10'0000), l);
    EXPECT_EQ(x.submit({1, kB, 10'0000, 10, Tif::Day, true}, l), 0u);
    const auto v = x.submit({1, kB, 9'9900, 10, Tif::Day, true}, l);
    ASSERT_NE(v, 0u);
    EXPECT_FALSE(x.cancel(v, 2, l));
    EXPECT_TRUE(x.cancel(v, 1, l));
    EXPECT_FALSE(x.book().order(v).has_value());
}

// ITCH sets the side of every P message to 'B', so a hidden print fills virtual orders on
// whichever side it reaches: bids at or above the print, asks at or below it.
TEST(ReplayExchange, HiddenPrintsIgnoreTheSideField) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    const auto bid = x.submit({1, kB, 10'0000, 40}, l);
    const auto ask = x.submit({1, hft::book::Side::Sell, 10'0100, 40}, l);
    feed(x, m.hidden('B', 10, 10'0100), l);  // at our ask
    EXPECT_EQ(l.filled(ask), 10u);
    EXPECT_EQ(l.filled(bid), 0u);
    feed(x, m.hidden('B', 10, 10'0050), l);  // midpoint: reaches neither
    EXPECT_EQ(l.filled(ask) + l.filled(bid), 10u);
    feed(x, m.hidden('B', 5, 9'9900), l);  // below our bid
    EXPECT_EQ(l.filled(bid), 5u);
}

// A mirrored real order leaves the replayed book and lives on as a virtual order at the same
// queue position: its own executions in the data become fills of the virtual order, its
// cancels reduce it, and orders behind it are untouched.
TEST(ReplayExchange, MirroredOrderReproducesItsExecutions) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    x.mirror(2, 7);
    feed(x, m.add(1, 'B', 100, 10'0000), l);
    feed(x, m.add(2, 'B', 300, 10'0000), l);  // mirrored
    feed(x, m.add(3, 'B', 100, 10'0000), l);
    const auto v = x.mirrored(2);
    ASSERT_NE(v, 0u);
    EXPECT_EQ(x.book().order(v)->owner, 7u);
    EXPECT_FALSE(x.book().order(2).has_value());
    EXPECT_EQ(x.queue_ahead(v), 100u);
    feed(x, m.exec(1, 60), l);
    EXPECT_EQ(x.queue_ahead(v), 40u);
    feed(x, m.exec(1, 40), l);
    EXPECT_EQ(x.queue_ahead(v), 0u);
    feed(x, m.exec(2, 120), l);  // the data executes the mirrored order
    EXPECT_EQ(l.filled(v), 120u);
    feed(x, m.cancel(2, 80), l);
    EXPECT_EQ(x.book().order(v)->qty, 100u);
    feed(x, m.exec(2, 100), l);
    EXPECT_EQ(l.filled(v), 220u);
    EXPECT_FALSE(x.book().order(v).has_value());
    EXPECT_EQ(x.divergence().diverted, 0u);
    EXPECT_EQ(x.book().order(3)->qty, 100u);
    EXPECT_TRUE(x.book().check());
}

TEST(ReplayExchange, QueueAheadMatchesTheBookWalk) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    for (std::uint64_t r = 1; r <= 20; ++r) feed(x, m.add(r, 'S', 10 * r, 10'0100), l);
    const auto v = x.submit({1, hft::book::Side::Sell, 10'0100, 50}, l);
    for (std::uint64_t r = 21; r <= 30; ++r) feed(x, m.add(r, 'S', 5, 10'0100), l);
    for (std::uint64_t r = 1; r <= 30; r += 3) feed(x, r % 2 ? m.cancel(r, 3) : m.del(r), l);
    feed(x, m.exec(2, 20), l);
    EXPECT_EQ(static_cast<std::int64_t>(x.queue_ahead(v)), x.book().queue_ahead(v));
}

TEST(ReplayExchange, MirroredExecutionsFillUnderTheConservativeRuleToo) {
    ReplayExchange<> x({}, FillRule::TradeThrough);
    Log l;
    Itch m;
    x.mirror(5, 3);
    feed(x, m.add(5, 'S', 100, 20'0000), l);
    feed(x, m.exec(5, 30), l);
    EXPECT_EQ(l.filled(x.mirrored(5)), 30u);
}

// Marks and features use the real book only: our virtual orders must not move the BBO.
TEST(ReplayExchange, RealBboIgnoresVirtualOrders) {
    ReplayExchange<> x({}, FillRule::Queue);
    Log l;
    Itch m;
    feed(x, m.add(1, 'B', 100, 10'0000), l);
    feed(x, m.add(2, 'S', 100, 10'0300), l);
    x.submit({1, kB, 10'0100, 50}, l);                     // improves the bid alone
    x.submit({1, hft::book::Side::Sell, 10'0300, 50}, l);  // joins the ask
    EXPECT_EQ(x.book().bbo(), (hft::book::Bbo{10'0100, 10'0300, 50, 150}));
    EXPECT_EQ(x.real_bbo(), (hft::book::Bbo{10'0000, 10'0300, 100, 100}));
}
