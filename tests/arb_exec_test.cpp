#include "exec/arb_exec.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace hft;
using namespace hft::exec;

namespace {

constexpr Ns kS = 1'000'000'000;

pm::MarketRules no_fees() {
    pm::MarketRules r;
    r.fees = false;
    r.tick = 100;
    r.min_qty = 5 * kShare;
    return r;
}

struct Legs {
    std::vector<pm::TokenBook> books;
    std::vector<pm::MarketRules> rules;
    std::vector<LegView> v;
    explicit Legs(std::size_t n, pm::MarketRules r = no_fees()) : books(n), rules(n, r) {
        for (std::size_t i = 0; i < n; ++i) {
            v.push_back({&books[i], &rules[i], 10 * kS, false, -1, -1});
            books[i].set(true, 100, 1), books[i].set(false, 9900, 1);  // far levels: books are two-sided
        }
    }
    void ask(std::size_t i, Px px, double size) { books[i].set(false, px, size); }
    void bid(std::size_t i, Px px, double size) { books[i].set(true, px, size); }
};

ArbParams params() {
    ArbParams p;
    p.latency_ticks = 0;
    return p;
}

Plan buy(const Legs& l, const ArbParams& p, Usd cash = 1'000'000 * kDollar, Usd room = 100'000 * kDollar) {
    return plan(true, l.v.data(), l.v.size(), 10 * kS, 9 * kS, cash, room, p);
}

}  // namespace

TEST(ArbExec, BuySidePassesWithGrossEdgeAndThinnestLegSize) {
    Legs l(3);
    l.ask(0, 3000, 50), l.ask(1, 3000, 20), l.ask(2, 3500, 80);  // 0.95 per set
    const Plan pl = buy(l, params());
    EXPECT_EQ(pl.why, ArbFilter::Pass);
    EXPECT_EQ(pl.size, 20 * kShare);
    EXPECT_EQ(pl.net, notional(500, 20 * kShare));  // 0.05 x 20 sets = 1 USD
}

TEST(ArbExec, NetEdgeTakesFeesAndAllowancesOff) {
    pm::MarketRules r = no_fees();
    r.fees = true, r.fee_rate_ppm = 50'000, r.fee_exp = 1;
    Legs l(2, r);
    l.ask(0, 4000, 100), l.ask(1, 5000, 100);  // gross 0.10 per set
    ArbParams p = params();
    p.slip_ticks = 1, p.latency_ticks = 1;
    const Plan pl = buy(l, p);
    ASSERT_EQ(pl.why, ArbFilter::Pass);
    const Usd fees = taker_fee(100 * kShare, 4000, r) + taker_fee(100 * kShare, 5000, r);  // 1.20 + 1.25 USD
    EXPECT_EQ(fees, 245 * kDollar / 100);
    EXPECT_EQ(pl.net, notional(1000, 100 * kShare) - fees - notional(2 * 2 * 100, 100 * kShare));  // 10 - 2.45 - 4
    p.min_edge = 400;  // 0.04 per set asked, 0.0355 left
    EXPECT_EQ(buy(l, p).why, ArbFilter::Edge);
}

TEST(ArbExec, FiltersEachRefuseInOrder) {
    Legs l(2);
    l.ask(0, 4000, 100), l.ask(1, 5000, 100);
    ArbParams p = params();
    p.min_age = 2 * kS;
    EXPECT_EQ(buy(l, p).why, ArbFilter::Young);  // open 1 s
    p.min_age = 0, p.fresh = kS / 2;
    l.v[1].updated = 9 * kS;
    EXPECT_EQ(buy(l, p).why, ArbFilter::Stale);  // leg 1 last seen 1 s ago
    l.v[1].updated = 10 * kS, l.v[0].frozen = true;
    EXPECT_EQ(buy(l, p).why, ArbFilter::Frozen);
    l.v[0].frozen = false, l.bid(0, 100, 0);
    EXPECT_EQ(buy(l, p).why, ArbFilter::OneSided);
    l.bid(0, 100, 1), l.v[1].own_ask = 5000;
    EXPECT_EQ(buy(l, p).why, ArbFilter::SelfCross);  // our own ask is the one we would buy
    l.v[1].own_ask = 5100;
    EXPECT_EQ(buy(l, p).why, ArbFilter::Pass);
    l.ask(1, 5000, 0), l.ask(1, 5000, 4.99);
    EXPECT_EQ(buy(l, p).why, ArbFilter::Size);  // below the 5-share minimum
    l.ask(1, 5000, 0), l.ask(1, 9000, 100), l.v[1].own_ask = -1;
    EXPECT_EQ(buy(l, p).why, ArbFilter::Edge);  // 1.30 per set
}

TEST(ArbExec, SizeIsBoundedByCashGroupRoomAndMaxSetInHundredths) {
    Legs l(2);
    l.ask(0, 4000, 1000), l.ask(1, 5000, 1000);  // 0.90 per set
    ArbParams p = params();
    EXPECT_EQ(buy(l, p, 90 * kDollar).size, 100 * kShare);           // 90 USD buys 100 sets
    EXPECT_EQ(buy(l, p, 100 * kDollar).size, 11111 * kShare / 100);  // 111.11, not 111.111
    EXPECT_EQ(buy(l, p, kDollar * 1000, 45 * kDollar).size, 50 * kShare);
    p.max_set = 7 * kShare;
    EXPECT_EQ(buy(l, p).size, 7 * kShare);
}

TEST(ArbExec, SellSideSplitsAndSellsAtBids) {
    Legs l(2);
    l.bid(0, 5500, 30), l.bid(1, 5000, 40);  // 1.05 per set
    const Plan pl = plan(false, l.v.data(), 2, 10 * kS, 9 * kS, 1'000'000 * kDollar, 100'000 * kDollar, params());
    EXPECT_EQ(pl.why, ArbFilter::Pass);
    EXPECT_EQ(pl.size, 30 * kShare);
    EXPECT_EQ(pl.net, notional(500, 30 * kShare));
    l.v[0].own_bid = 5500;
    EXPECT_EQ(plan(false, l.v.data(), 2, 10 * kS, 9 * kS, 1'000'000 * kDollar, 100'000 * kDollar, params()).why,
              ArbFilter::SelfCross);
}

TEST(ArbExec, WalkCostsLevelsAndRefusesBeyondDepth) {
    Legs l(1);
    l.ask(0, 4000, 10), l.ask(0, 4100, 20), l.bid(0, 3900, 5), l.bid(0, 3800, 50);
    Px limit = 0;
    EXPECT_EQ(walk(l.v[0], true, 25 * kShare, &limit), notional(4000, 10 * kShare) + notional(4100, 15 * kShare));
    EXPECT_EQ(limit, 4100);
    EXPECT_EQ(walk(l.v[0], false, 6 * kShare, &limit), notional(3900, 5 * kShare) + notional(3800, kShare));
    EXPECT_EQ(limit, 3800);
    EXPECT_EQ(walk(l.v[0], true, 32 * kShare, &limit), -1);  // 31 there, one of them the far 0.99
    l.v[0].collar = 50;  // risk lets an order go 0.005 past the best: the 0.41 level is out of reach
    EXPECT_EQ(walk(l.v[0], true, 11 * kShare, &limit), -1);
    EXPECT_EQ(walk(l.v[0], true, 10 * kShare, &limit), notional(4000, 10 * kShare));
    l.v[0].collar = kPxOne;
    Legs one(1);
    one.ask(0, 4000, 100), one.bid(0, 100, 0);  // no bids: risk refuses any order on a one-sided book (R3)
    EXPECT_EQ(walk(one.v[0], true, kShare, &limit), -1);
    // What we already took at 0.40 is not there for us until the feed updates the level.
    l.v[0].taken = [](const void*, std::uint32_t, bool bid, Px px) -> Qty { return !bid && px == 4000 ? 6 * kShare : 0; };
    EXPECT_EQ(walk(l.v[0], true, 26 * kShare, &limit), -1);
    EXPECT_EQ(walk(l.v[0], true, 24 * kShare, &limit), notional(4000, 4 * kShare) + notional(4100, 20 * kShare));
}

TEST(ArbExec, IncompleteSetTakesTheCheaperOfCompletingAndUnwinding) {
    Legs l(2);
    // Bought 100 of leg 0 at 0.40 and 60 of leg 1 at 0.50: 70 USD out.
    const Qty held[2] = {100 * kShare, 60 * kShare};
    const Usd spent = -(notional(4000, 100 * kShare) + notional(5000, 60 * kShare));
    l.ask(1, 5200, 100);  // completing: 40 more of leg 1 at 0.52 -> 100 sets
    l.bid(0, 3000, 100);  // unwinding: sell 40 of leg 0 at 0.30 -> 60 sets
    Choice c = choose(held, l.v.data(), 2, spent, 1000 * kDollar);
    EXPECT_EQ(c.action, ArbAction::Complete);
    EXPECT_EQ(c.value, spent - notional(5200, 40 * kShare) + notional(kPxOne, 100 * kShare));  // +9.2 USD
    l.ask(1, 5200, 0), l.ask(1, 8000, 100);  // completing now costs 0.80
    c = choose(held, l.v.data(), 2, spent, 1000 * kDollar);
    EXPECT_EQ(c.action, ArbAction::Unwind);
    EXPECT_EQ(c.value, spent + notional(3000, 40 * kShare) + notional(kPxOne, 60 * kShare));  // +2 USD
    const Qty even[2] = {60 * kShare, 60 * kShare};
    EXPECT_EQ(choose(even, l.v.data(), 2, spent, 1000 * kDollar).action, ArbAction::None);
}

TEST(ArbExec, CompletingAndUnwindingPayTakerFees) {
    pm::MarketRules r = no_fees();
    r.fees = true;
    Legs l(2, r);
    const Qty held[2] = {100 * kShare, 60 * kShare};
    l.ask(1, 5000, 100), l.bid(0, 4000, 100);
    const Choice c = choose(held, l.v.data(), 2, 0, 1000 * kDollar);
    const Usd complete = -notional(5000, 40 * kShare) - taker_fee(40 * kShare, 5000, r) + notional(kPxOne, 100 * kShare);
    const Usd unwind = notional(4000, 40 * kShare) - taker_fee(40 * kShare, 4000, r) + notional(kPxOne, 60 * kShare);
    EXPECT_EQ(c.value, complete > unwind ? complete : unwind);
}

TEST(ArbExec, LossBeyondTheAttemptBoundFreezes) {
    Legs l(2);
    const Qty held[2] = {100 * kShare, 0};
    const Usd spent = -notional(4000, 100 * kShare);  // 40 USD out
    l.ask(1, 9900, 100);                                // completing: 99 USD more for 100 USD
    l.bid(0, 1000, 100);                                // unwinding: 10 USD back
    EXPECT_EQ(choose(held, l.v.data(), 2, spent, 31 * kDollar).action, ArbAction::Unwind);  // -30 within 31
    EXPECT_EQ(choose(held, l.v.data(), 2, spent, 29 * kDollar).action, ArbAction::Freeze);
    l.bid(0, 1000, 0), l.ask(1, 9900, 0);  // no depth either way
    EXPECT_EQ(choose(held, l.v.data(), 2, spent, 1000 * kDollar).action, ArbAction::Freeze);
}
