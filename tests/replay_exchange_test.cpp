#include "engine/replay_exchange.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace hft::engine;

namespace {

// Encodes ITCH messages for one symbol (locate 1).
struct Itch {
    std::vector<std::uint8_t> b;
    Itch& head(char t) {
        b.clear();
        b.push_back(static_cast<std::uint8_t>(t));
        return n(1, 2).n(0, 2).n(34'200'000'000'000, 6);
    }
    Itch& n(std::uint64_t v, int bytes) {
        for (int s = (bytes - 1) * 8; s >= 0; s -= 8)
            b.push_back(static_cast<std::uint8_t>(v >> s));
        return *this;
    }
    Itch& c(char v) { return n(static_cast<std::uint8_t>(v), 1); }
    Itch& stock() { return n(0x5445535420202020ull, 8); }  // "TEST    "

    Itch& add(std::uint64_t ref, char side, std::uint32_t qty, std::uint32_t px) {
        return head('A').n(ref, 8).c(side).n(qty, 4).stock().n(px, 4);
    }
    Itch& exec(std::uint64_t ref, std::uint32_t qty) {
        return head('E').n(ref, 8).n(qty, 4).n(9, 8);
    }
    Itch& del(std::uint64_t ref) { return head('D').n(ref, 8); }
    Itch& hidden(char side, std::uint32_t qty, std::uint32_t px) {
        return head('P').n(0, 8).c(side).n(qty, 4).stock().n(px, 4).n(9, 8);
    }
};

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
