#include "engine/matching.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <vector>

#include "ref_engine.hpp"

using namespace hft::engine;

namespace {

bool same(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.side == b.side && a.maker == b.maker &&
           a.owner == b.owner && a.ref == b.ref && a.price == b.price && a.qty == b.qty &&
           a.fee == b.fee && a.match == b.match;
}

struct Log {
    std::vector<Event> ev;
    void operator()(const Event& e) { ev.push_back(e); }
};

constexpr Side kB = Side::Buy, kS = Side::Sell;
constexpr std::uint64_t R0 = MatchingEngine<>::kFirstRef;

Config fees() {
    Config c;
    c.maker_rebate = 2000;  // $0.0020
    c.taker_fee = 3000;     // $0.0030
    return c;
}

}  // namespace

TEST(Matching, SweepsLevelsInPriceTimeOrderWithFees) {
    MatchingEngine<> m(fees());
    Log l;
    m.submit({1, kS, 10'0100, 100}, l);
    m.submit({2, kS, 10'0000, 50}, l);
    m.submit({3, kS, 10'0000, 70}, l);
    l.ev.clear();
    m.submit({4, kB, 10'0100, 200}, l);
    // 50 @ 10.00 (owner 2), 70 @ 10.00 (owner 3), then 80 of 100 @ 10.01 (owner 1).
    ASSERT_EQ(l.ev.size(), 7u);
    EXPECT_EQ(l.ev[0].type, EventType::Accepted);
    const std::uint32_t own[3] = {2, 3, 1}, px[3] = {10'0000, 10'0000, 10'0100},
                        q[3] = {50, 70, 80};
    for (int k = 0; k < 3; ++k) {
        const Event& mk = l.ev[1 + 2 * k];
        const Event& tk = l.ev[2 + 2 * k];
        EXPECT_TRUE(mk.maker);
        EXPECT_EQ(mk.owner, own[k]);
        EXPECT_EQ(mk.price, px[k]);
        EXPECT_EQ(mk.qty, q[k]);
        EXPECT_EQ(mk.fee, -2000 * static_cast<std::int64_t>(q[k]));
        EXPECT_FALSE(tk.maker);
        EXPECT_EQ(tk.owner, 4u);
        EXPECT_EQ(tk.fee, 3000 * static_cast<std::int64_t>(q[k]));
        EXPECT_EQ(tk.match, mk.match);
    }
    EXPECT_EQ(m.book().bbo(), (hft::book::Bbo{0, 10'0100, 0, 20}));
}

TEST(Matching, IocAndMarketRemaindersCancel) {
    MatchingEngine<> m({});
    Log l;
    m.submit({1, kS, 10'0000, 30}, l);
    l.ev.clear();
    m.submit({2, kB, 10'0000, 50, Tif::Ioc}, l);
    ASSERT_EQ(l.ev.back().type, EventType::Cancelled);
    EXPECT_EQ(l.ev.back().reason, Reason::Ioc);
    EXPECT_EQ(l.ev.back().qty, 20u);
    m.submit({1, kS, 11'0000, 30}, l);
    l.ev.clear();
    m.submit({2, kB, 0, 50, Tif::Ioc}, l);  // market order
    EXPECT_EQ(l.ev[1].price, 11'0000u);
    EXPECT_EQ(l.ev.back().qty, 20u);
    l.ev.clear();
    m.submit({2, kB, 0, 50, Tif::Day}, l);  // market orders must be IOC
    EXPECT_EQ(l.ev[0].reason, Reason::BadPrice);
}

TEST(Matching, PostOnlyPolicies) {
    for (LockPolicy p : {LockPolicy::Reject, LockPolicy::Reprice, LockPolicy::AllowLock}) {
        Config c;
        c.lock = p;
        MatchingEngine<> m(c);
        Log l;
        m.submit({1, kS, 10'0000, 100}, l);
        l.ev.clear();
        m.submit({2, kB, 10'0000, 10, Tif::Day, true}, l);  // would lock
        m.submit({2, kB, 10'0200, 10, Tif::Day, true}, l);  // would cross
        if (p == LockPolicy::Reject) {
            EXPECT_EQ(l.ev[0].reason, Reason::WouldLock);
            EXPECT_EQ(l.ev[1].reason, Reason::WouldCross);
            EXPECT_EQ(m.book().bbo().bid_px, 0u);
        } else if (p == LockPolicy::Reprice) {
            EXPECT_EQ(l.ev[0].price, 9'9900u);
            EXPECT_EQ(l.ev[1].price, 9'9900u);
            EXPECT_EQ(m.book().bbo(), (hft::book::Bbo{9'9900, 10'0000, 20, 100}));
        } else {
            EXPECT_EQ(l.ev[1].price, 10'0000u);
            EXPECT_EQ(m.book().bbo(), (hft::book::Bbo{10'0000, 10'0000, 20, 100}));
        }
    }
}

TEST(Matching, SelfTradePrevention) {
    for (Stp s : {Stp::CancelNewest, Stp::CancelOldest, Stp::CancelBoth, Stp::None}) {
        Config c;
        c.stp = s;
        MatchingEngine<> m(c);
        Log l;
        m.submit({7, kS, 10'0000, 100}, l);
        m.submit({8, kS, 10'0100, 100}, l);
        l.ev.clear();
        m.submit({7, kB, 10'0100, 150}, l);
        const auto bbo = m.book().bbo();
        if (s == Stp::CancelNewest) {
            EXPECT_EQ(l.ev.back().reason, Reason::SelfTrade);
            EXPECT_EQ(l.ev.back().ref, R0 + 2);
            EXPECT_EQ(bbo, (hft::book::Bbo{0, 10'0000, 0, 100}));
        } else if (s == Stp::CancelOldest) {
            EXPECT_EQ(l.ev[1].ref, R0);  // resting order cancelled, then trades with owner 8
            EXPECT_EQ(l.ev[2].owner, 8u);
            EXPECT_EQ(bbo, (hft::book::Bbo{10'0100, 0, 50, 0}));
        } else if (s == Stp::CancelBoth) {
            EXPECT_EQ(l.ev.size(), 3u);
            EXPECT_EQ(bbo, (hft::book::Bbo{0, 10'0100, 0, 100}));
        } else {
            EXPECT_EQ(l.ev[1].owner, 7u);  // trades with itself
            EXPECT_EQ(bbo, (hft::book::Bbo{0, 10'0100, 0, 50}));
        }
    }
}

TEST(Matching, TickGridAndHalfPenny) {
    MatchingEngine<> penny({});
    Log l;
    EXPECT_EQ(penny.submit({1, kB, 10'0050, 10}, l), 0u);
    EXPECT_EQ(l.ev.back().reason, Reason::BadPrice);
    Config c;
    c.tick = 50;
    MatchingEngine<> half(c);
    EXPECT_NE(half.submit({1, kB, 10'0050, 10}, l), 0u);
    EXPECT_EQ(half.submit({1, kB, 10'0025, 10}, l), 0u);
    EXPECT_EQ(half.book().bbo().bid_px, 10'0050u);
}

TEST(Matching, ReducePreservesPriorityReplaceLosesIt) {
    MatchingEngine<> m({});
    Log l;
    const auto a = m.submit({1, kS, 10'0000, 100}, l);
    m.submit({2, kS, 10'0000, 100}, l);
    m.reduce(a, 1, 40, l);
    l.ev.clear();
    m.submit({3, kB, 10'0000, 10, Tif::Ioc}, l);
    EXPECT_EQ(l.ev[1].owner, 1u);  // still first
    const auto a2 = m.replace(a, 1, 10'0000, 50, false, l);
    l.ev.clear();
    m.submit({3, kB, 10'0000, 10, Tif::Ioc}, l);
    EXPECT_EQ(l.ev[1].owner, 2u);      // replaced order went to the back
    EXPECT_FALSE(m.cancel(a2, 2, l));  // wrong owner
    EXPECT_TRUE(m.cancel(a2, 1, l));
}

TEST(Matching, OddLotsTradeInTimePriority) {
    MatchingEngine<> m({});
    Log l;
    m.submit({1, kS, 10'0000, 37}, l);  // odd lot
    m.submit({2, kS, 10'0000, 200}, l);
    l.ev.clear();
    m.submit({3, kB, 10'0000, 50, Tif::Ioc}, l);
    EXPECT_EQ(l.ev[1].owner, 1u);
    EXPECT_EQ(l.ev[1].qty, 37u);
    EXPECT_EQ(l.ev[3].owner, 2u);
    EXPECT_EQ(l.ev[3].qty, 13u);
}

namespace {

struct Cmd {
    std::uint8_t kind, owner, flags;
    std::uint16_t price, qty;
    std::uint8_t pick;
};

}  // namespace

namespace rc {
template <>
struct Arbitrary<Cmd> {
    static Gen<Cmd> arbitrary() {
        return gen::build<Cmd>(gen::set(&Cmd::kind), gen::set(&Cmd::owner), gen::set(&Cmd::flags),
                               gen::set(&Cmd::price), gen::set(&Cmd::qty), gen::set(&Cmd::pick));
    }
};
}  // namespace rc

// Random command sequences: the engine and the naive reference emit identical events.
RC_GTEST_PROP(MatchingProp, MatchesReferenceEngine, (const std::vector<Cmd>& cmds)) {
    Config c;
    c.maker_rebate = 2000;
    c.taker_fee = 3000;
    c.stp = static_cast<Stp>(*rc::gen::inRange(0, 4));
    c.lock = static_cast<LockPolicy>(*rc::gen::inRange(0, 3));
    MatchingEngine<> m(c, 4);
    ref::RefEngine r(c);
    Log a, b;
    std::vector<std::uint64_t> refs;
    for (const Cmd& k : cmds) {
        const std::uint32_t owner = k.owner % 4;
        const Side side = k.flags & 1 ? Side::Sell : Side::Buy;
        // Prices cluster within 20 ticks of $50, with a few off the grid or market.
        std::uint32_t px = 50'0000 + (k.price % 41) * 100 - 20 * 100;
        if (k.flags & 16) px += 37;
        if ((k.flags & 0xe0) == 0xe0) px = 0;
        const std::uint32_t qty = k.qty % 300;
        const Tif tif = k.flags & 2 || px == 0 ? Tif::Ioc : Tif::Day;
        const bool post = k.flags & 4;
        const std::uint64_t ref = refs.empty() ? 0 : refs[k.pick % refs.size()];
        switch (k.kind % 8) {
            case 0:
                m.cancel(ref, owner, a);
                r.cancel(ref, owner, b);
                break;
            case 1:
                m.reduce(ref, owner, qty, a);
                r.reduce(ref, owner, qty, b);
                break;
            case 2:
                refs.push_back(m.replace(ref, owner, px, qty, post, a));
                r.replace(ref, owner, px, qty, post, b);
                break;
            default:
                refs.push_back(m.submit({owner, side, px, qty, tif, post}, a));
                r.submit({owner, side, px, qty, tif, post}, b);
        }
        RC_ASSERT(a.ev.size() == b.ev.size());
        for (std::size_t i = 0; i < a.ev.size(); ++i) RC_ASSERT(same(a.ev[i], b.ev[i]));
        RC_ASSERT(m.book().check());
    }
}
