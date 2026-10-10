#include "exec/ledger.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <vector>

using namespace hft::exec;

namespace {
constexpr Usd kCap = 1'000'000 * kDollar;
constexpr Usd cents(std::int64_t c) { return c * kDollar / 100; }
}  // namespace

TEST(Ledger, BuyThenPartSellRealisesAgainstCostBasis) {
    Ledger l(kCap);
    l.ensure(1);
    l.fill(1, 0, Side::Buy, 4000, 10 * kShare, cents(1));  // 10 @ 0.40, fee 0.01
    l.fill(2, 0, Side::Sell, 5000, 4 * kShare, cents(1));  // 4 @ 0.50, fee 0.01
    EXPECT_EQ(l.pos(0), 6 * kShare);
    EXPECT_EQ(l.cost(0), cents(240));                  // 6 left at 0.40
    EXPECT_EQ(l.realised(), cents(40));                 // 4 * (0.50 - 0.40)
    EXPECT_EQ(l.fees(), cents(2));
    EXPECT_EQ(l.cash(), kCap - cents(400) - cents(1) + cents(200) - cents(1));
    const Px marks[] = {4500};
    EXPECT_EQ(l.unrealised(marks), cents(30));          // 6 * (0.45 - 0.40)
    EXPECT_EQ(l.total(marks), cents(40) + cents(30) - cents(2));
    EXPECT_TRUE(l.identity_holds());
}

TEST(Ledger, MergingAPairReturnsADollarPerSetAtOnce) {
    Ledger l(kCap);
    l.ensure(2);
    l.fill(1, 0, Side::Buy, 4500, 5 * kShare, 0);  // Yes
    l.fill(2, 1, Side::Buy, 5000, 5 * kShare, 0);  // No
    l.merge(0, 1, 5 * kShare);
    EXPECT_EQ(l.pos(0), 0);
    EXPECT_EQ(l.pos(1), 0);
    EXPECT_EQ(l.cash(), kCap + cents(25));  // paid 4.75 for sets worth 5.00
    EXPECT_EQ(l.realised(), cents(25));
    EXPECT_TRUE(l.identity_holds());
}

TEST(Ledger, SplittingCreatesAPairForADollar) {
    Ledger l(kCap);
    l.ensure(2);
    l.split(0, 1, 3 * kShare);
    EXPECT_EQ(l.pos(0), 3 * kShare);
    EXPECT_EQ(l.pos(1), 3 * kShare);
    EXPECT_EQ(l.cash(), kCap - cents(300));
    EXPECT_EQ(l.cost(0) + l.cost(1), cents(300));
    EXPECT_TRUE(l.identity_holds());
}

TEST(Ledger, ResolutionSettlesAtOneOrZero) {
    Ledger l(kCap);
    l.ensure(2);
    l.fill(1, 0, Side::Buy, 6000, 3 * kShare, 0);
    l.fill(2, 1, Side::Buy, 3000, 2 * kShare, 0);
    l.resolve(0, true);
    l.resolve(1, false);
    EXPECT_EQ(l.pos(0), 0);
    EXPECT_EQ(l.realised(), cents(120) - cents(60));  // +3 * 0.40, -2 * 0.30
    EXPECT_EQ(l.cash(), kCap - cents(180) - cents(60) + cents(300));
    EXPECT_TRUE(l.identity_holds());
}

TEST(Ledger, AFailedSettlementUndoesTheFill) {
    Ledger l(kCap);
    l.ensure(1);
    l.fill(7, 0, Side::Buy, 4000, 10 * kShare, cents(1));
    EXPECT_EQ(l.confirmed(0), 0);
    EXPECT_TRUE(l.failed(7));
    EXPECT_EQ(l.pos(0), 0);
    EXPECT_EQ(l.cash(), kCap);
    EXPECT_EQ(l.cost(0), 0);
    EXPECT_EQ(l.realised(), 0);
    EXPECT_EQ(l.fees(), 0);
    EXPECT_FALSE(l.failed(7));  // already resolved
    EXPECT_FALSE(l.settled(99));
    EXPECT_EQ(l.deficits(), 0u);
}

TEST(Ledger, FailureAfterTheSharesWereSoldIsADeficit) {
    Ledger l(kCap);
    l.ensure(1);
    l.fill(1, 0, Side::Buy, 4000, 10 * kShare, 0);
    l.fill(2, 0, Side::Sell, 4500, 10 * kShare, 0);
    EXPECT_TRUE(l.settled(2));
    EXPECT_TRUE(l.failed(1));
    EXPECT_EQ(l.deficits(), 1u);
    EXPECT_EQ(l.pos(0), -10 * kShare);  // shares sold that we never received
    EXPECT_TRUE(l.identity_holds());
}

TEST(Ledger, SettlementMovesFillsToConfirmed) {
    Ledger l(kCap);
    l.ensure(1);
    l.fill(1, 0, Side::Buy, 4000, 10 * kShare, 0);
    l.fill(2, 0, Side::Sell, 4000, 3 * kShare, 0);
    EXPECT_TRUE(l.settled(1));
    EXPECT_EQ(l.confirmed(0), 10 * kShare);
    EXPECT_TRUE(l.settled(2));
    EXPECT_EQ(l.confirmed(0), 7 * kShare);
    EXPECT_EQ(l.pending_fills(), 0u);
}

TEST(Ledger, ReservationsLimitWhatIsAvailable) {
    Ledger l(kCap);
    l.ensure(1);
    l.fill(1, 0, Side::Buy, 4000, 10 * kShare, 0);
    l.reserve_cash(cents(100));
    l.reserve_pos(0, 4 * kShare);
    EXPECT_EQ(l.available_cash(), kCap - cents(400) - cents(100));
    EXPECT_EQ(l.available_pos(0), 6 * kShare);
    l.release_cash(cents(100));
    l.release_pos(0, 4 * kShare);
    EXPECT_EQ(l.available_cash(), l.cash());
    EXPECT_EQ(l.available_pos(0), 10 * kShare);
}

TEST(Ledger, CapitalTimeAddsLockedMoneyTimesDuration) {
    Ledger l(kCap);
    l.ensure(1);
    l.tick(0);
    l.fill(1, 0, Side::Buy, 5000, 100 * kShare, 0);  // 50 USD in the position
    l.tick(86'400'000'000'000);                      // one day later
    EXPECT_DOUBLE_EQ(l.capital_usd_days(), 50.0);
}

// Random operations against an independent cash count: the identity holds after every step and
// cash equals capital plus every cash flow.
RC_GTEST_PROP(Ledger, IdentityAndCashHoldUnderAnySequence, ()) {
    constexpr std::uint32_t kTok = 4;
    Ledger l(kCap);
    l.ensure(kTok);
    Usd cash = kCap;
    std::vector<std::uint64_t> pending;
    std::uint64_t next_id = 1;
    const int n = *rc::gen::inRange(1, 300);
    for (int i = 0; i < n; ++i) {
        const int op = *rc::gen::inRange(0, 8);
        const auto t = *rc::gen::inRange<std::uint32_t>(0, kTok);
        const Px p = *rc::gen::inRange<Px>(1, kPxOne);
        const Usd fee = *rc::gen::inRange<Usd>(0, cents(5));
        if (op <= 1) {
            const Qty q = *rc::gen::inRange<Qty>(1, 50 * kShare);
            l.fill(next_id, t, Side::Buy, p, q, fee);
            pending.push_back(next_id++);
            cash -= notional(p, q) + fee;
        } else if (op <= 3 && l.pos(t) > 0) {
            const Qty q = *rc::gen::inRange<Qty>(1, l.pos(t) + 1);
            l.fill(next_id, t, Side::Sell, p, q, fee);
            pending.push_back(next_id++);
            cash += notional(p, q) - fee;
        } else if (op == 4 && !pending.empty()) {
            RC_ASSERT(l.settled(pending.back()));
            pending.pop_back();
        } else if (op == 5 && !pending.empty()) {
            const std::uint64_t id = pending.front();
            const Ledger::Fill f = *l.pending(id);
            RC_ASSERT(l.failed(id));
            pending.erase(pending.begin());
            cash += f.side == Side::Buy ? notional(f.px, f.qty) + f.fee : -notional(f.px, f.qty) + f.fee;
        } else if (op == 6) {
            const Qty q = *rc::gen::inRange<Qty>(1, 20 * kShare);
            l.split(0, 1, q);
            cash -= notional(kPxOne, q);
        } else if (op == 7) {
            const Qty q = std::min(l.pos(0), l.pos(1));
            if (q > 0) {
                l.merge(0, 1, q);
                cash += notional(kPxOne, q);
            }
        }
        RC_ASSERT(l.identity_holds());
        RC_ASSERT(l.cash() == cash);
        Px marks[kTok];
        for (auto& m : marks) m = *rc::gen::inRange<Px>(0, kPxOne + 1);
        Usd by_cash = l.cash() - kCap;
        for (std::uint32_t k = 0; k < kTok; ++k) by_cash += notional(marks[k], l.pos(k));
        RC_ASSERT(l.total(marks) == by_cash);
    }
}
