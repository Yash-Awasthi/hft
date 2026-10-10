#include "exec/risk.hpp"

#include <gtest/gtest.h>

using namespace hft::exec;
using R = Reject;

namespace {

constexpr Usd kCap = 1'000'000 * kDollar;
constexpr Ns kSec = 1'000'000'000;

struct Fixture {
    Ledger ledger{kCap};
    Exposure exp;
    hft::pm::MarketRules rules;
    RiskLimits lim;
    Risk risk;
    MarketView mv;

    Fixture() : risk(lim) {
        ledger.ensure(4);
        exp.ensure(4, 2);
        risk.ensure(4);
        for (std::uint32_t t = 0; t < 4; ++t) risk.set_group(t, t / 2), exp.set_group(t, t / 2), ledger.set_group(t, t / 2);
        rules.tick = 10;  // 0.001
        mv = {4000, 4100, true, false, &rules};
    }
    OrderIntent buy(Px px, Qty q, std::uint32_t t = 0) { return {t, Side::Buy, Tif::Gtc, false, 0, px, q, 0}; }
    OrderIntent sell(Px px, Qty q, std::uint32_t t = 0) { return {t, Side::Sell, Tif::Gtc, false, 0, px, q, 0}; }
    R check(const OrderIntent& o, Ns now = kSec) { return risk.check(o, mv, exp, ledger, now); }
};

}  // namespace

TEST(Risk, TickSizeAndPriceBounds) {
    Fixture f;
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.buy(4005, 10 * kShare)), R::Tick);     // off the 0.001 grid
    EXPECT_EQ(f.check(f.buy(0, 10 * kShare)), R::Tick);
    EXPECT_EQ(f.check(f.buy(kPxOne, 10 * kShare)), R::Tick);
    EXPECT_EQ(f.check(f.buy(4000, 5 * kShare)), R::None);      // exactly the minimum
    EXPECT_EQ(f.check(f.buy(4000, 5 * kShare - 10'000)), R::Size);  // 4.99
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare + 5'000)), R::Size);  // 10.005: sizes have 2 decimals
    EXPECT_EQ(f.check(f.buy(4000, f.lim.max_order_qty + 10'000)), R::Size);
}

TEST(Risk, CollarNeedsAFreshTwoSidedBook) {
    Fixture f;
    const Px c = f.lim.collar_ticks * f.rules.tick;
    EXPECT_EQ(f.check(f.buy(4100 + c, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.buy(4100 + c + 10, 10 * kShare)), R::Collar);
    f.ledger.fill(1, 0, Side::Buy, 4000, 100 * kShare, 0);
    EXPECT_EQ(f.check(f.sell(4000 - c, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.sell(4000 - c - 10, 10 * kShare)), R::Collar);
    f.mv.fresh = false;
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare)), R::NoBook);
    f.mv = {-1, 4100, true, false, &f.rules};
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare)), R::NoBook);
}

TEST(Risk, NoSelfCross) {
    Fixture f;
    f.ledger.fill(1, 0, Side::Buy, 4000, 100 * kShare, 0);
    f.exp.on_open(f.sell(4060, 10 * kShare));
    EXPECT_EQ(f.check(f.buy(4050, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.buy(4060, 10 * kShare)), R::SelfCross);
    f.exp.on_open(f.buy(4020, 10 * kShare));
    EXPECT_EQ(f.check(f.sell(4030, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.sell(4020, 10 * kShare)), R::SelfCross);
}

TEST(Risk, SellsOnlyWhatIsHeldAndNotAlreadyOffered) {
    Fixture f;
    EXPECT_EQ(f.check(f.sell(4000, 10 * kShare)), R::Position);  // long only
    f.ledger.fill(1, 0, Side::Buy, 4000, 20 * kShare, 0);
    EXPECT_EQ(f.check(f.sell(4000, 20 * kShare)), R::None);
    f.ledger.reserve_pos(0, 10 * kShare);  // an open sell already offers 10
    EXPECT_EQ(f.check(f.sell(4000, 10 * kShare)), R::None);
    EXPECT_EQ(f.check(f.sell(4000, 10 * kShare + 10'000)), R::Position);
}

TEST(Risk, NotionalCapsCountOpenBuysPerTokenGroupAndGross) {
    Fixture f;
    f.lim.token_cap = 1000 * kDollar, f.lim.group_cap = 1500 * kDollar, f.lim.gross_cap = 2500 * kDollar;
    f.risk = Risk(f.lim);
    f.risk.ensure(4);
    for (std::uint32_t t = 0; t < 4; ++t) f.risk.set_group(t, t / 2);
    f.ledger.fill(1, 0, Side::Buy, 5000, 1000 * kShare, 0);  // 500 USD in token 0
    f.exp.on_open(f.buy(4000, 1000 * kShare));                // 400 USD open
    EXPECT_EQ(f.check(f.buy(4000, 250 * kShare)), R::None);   // 900 + 100 = 1000: at the cap
    EXPECT_EQ(f.check(f.buy(4000, 250 * kShare + 10'000)), R::Position);
    EXPECT_EQ(f.check(f.buy(4000, 1500 * kShare, 1)), R::None);   // group: 900 + 600 = 1500
    EXPECT_EQ(f.check(f.buy(4000, 1500 * kShare + 10'000, 1)), R::Group);
    f.ledger.fill(2, 2, Side::Buy, 5000, 1800 * kShare, 0);       // 900 USD in group 1
    EXPECT_EQ(f.check(f.buy(4000, 1750 * kShare, 3)), R::Group);  // group 1 would be 1600
    f.ledger.fill(3, 3, Side::Buy, 5000, 1000 * kShare, 0);       // gross now 1900 held + 400 open
    EXPECT_EQ(f.check(f.buy(4000, 500 * kShare, 1)), R::None);    // 2300 + 200 = 2500: at the cap
    EXPECT_EQ(f.check(f.buy(4000, 500 * kShare + 10'000, 1)), R::Gross);
}

TEST(Risk, CashCoversNotionalAndTheWorstCaseFee) {
    Fixture f;
    f.lim.token_cap = f.lim.group_cap = f.lim.gross_cap = 10'000'000 * kDollar;
    f.lim.max_order_qty = 10'000'000 * kShare;
    f.risk = Risk(f.lim);
    f.risk.ensure(4);
    f.ledger.reserve_cash(kCap - 1000 * kDollar);  // 1000 USD left
    f.mv = {4990, 5000, true, false, &f.rules};
    f.rules.fees = false;
    EXPECT_EQ(f.check(f.buy(5000, 2000 * kShare)), R::None);  // exactly 1000 USD
    f.rules.fees = true;                                       // + taker fee: no longer fits
    EXPECT_EQ(f.check(f.buy(5000, 2000 * kShare)), R::Cash);
}

TEST(Risk, OrderAndCancelRatesAreBucketed) {
    Fixture f;
    f.lim.order_burst = 3, f.lim.order_rate_per_s = 1;
    f.lim.sustained_burst = 1000, f.lim.sustained_rate_per_s = 1000;
    f.lim.cancel_burst = 2, f.lim.cancel_rate_per_s = 1;
    f.risk = Risk(f.lim);
    f.risk.ensure(4);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(f.check(f.buy(4000, 10 * kShare), kSec), R::None);
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare), kSec), R::Throttle);
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare), 2 * kSec), R::None);  // one token back after 1 s
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare), 2 * kSec), R::Throttle);
    EXPECT_TRUE(f.risk.check_cancel(kSec));
    EXPECT_TRUE(f.risk.check_cancel(kSec));
    EXPECT_FALSE(f.risk.check_cancel(kSec));
}

TEST(Risk, OpenOrderCounts) {
    Fixture f;
    f.lim.max_open_per_token = 2;
    f.risk = Risk(f.lim);
    f.risk.ensure(4);
    f.exp.on_open(f.buy(3000, 10 * kShare));
    EXPECT_EQ(f.check(f.buy(3010, 10 * kShare)), R::None);
    f.exp.on_open(f.buy(3010, 10 * kShare));
    EXPECT_EQ(f.check(f.buy(3020, 10 * kShare)), R::TooMany);
    f.exp.on_close(f.buy(3010, 10 * kShare), 10 * kShare);
    EXPECT_EQ(f.check(f.buy(3020, 10 * kShare)), R::None);
}

TEST(Risk, KillSwitchBlocksOrdersNotCancelsAndHasNoReset) {
    Fixture f;
    f.mv.frozen = true;
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare)), R::Frozen);
    f.mv.frozen = false;
    f.risk.kill(KillReason::Operator, 5);
    EXPECT_TRUE(f.risk.killed());
    EXPECT_EQ(f.risk.kill_reason(), KillReason::Operator);
    EXPECT_EQ(f.check(f.buy(4000, 10 * kShare)), R::Killed);
    EXPECT_TRUE(f.risk.check_cancel(kSec));
    f.risk.kill(KillReason::Loss, 9);  // the first reason is kept
    EXPECT_EQ(f.risk.kill_reason(), KillReason::Operator);
}

TEST(Risk, PostTradeTripsOnLossDeficitAndRejectSpikes) {
    {
        Fixture f;
        f.lim.daily_stop = 100 * kDollar;
        f.risk = Risk(f.lim);
        f.risk.ensure(4);
        f.ledger.fill(1, 0, Side::Buy, 5000, 1000 * kShare, 0);  // 500 USD
        const Px mid[] = {4950, 0, 0, 0}, liq[] = {4000, 0, 0, 0};
        f.risk.on_tick(f.ledger, mid, mid, kSec);  // -10 USD
        EXPECT_FALSE(f.risk.killed());
        f.risk.on_tick(f.ledger, mid, liq, kSec);  // liquidation mark: -100 USD -> at the stop, not past
        EXPECT_FALSE(f.risk.killed());
        const Px liq2[] = {3990, 0, 0, 0};
        f.risk.on_tick(f.ledger, mid, liq2, kSec);  // -101 USD
        EXPECT_EQ(f.risk.kill_reason(), KillReason::Loss);
    }
    {
        Fixture f;
        f.ledger.fill(1, 0, Side::Buy, 4000, 10 * kShare, 0);
        f.ledger.fill(2, 0, Side::Sell, 4000, 10 * kShare, 0);
        f.ledger.failed(1);
        const Px m[] = {0, 0, 0, 0};
        f.risk.on_tick(f.ledger, m, m, kSec);
        EXPECT_EQ(f.risk.kill_reason(), KillReason::Deficit);
    }
    {
        Fixture f;
        f.lim.reject_spike = 3, f.lim.reject_window = 10 * kSec;
        f.risk = Risk(f.lim);
        f.risk.ensure(4);
        f.risk.on_venue_reject(1 * kSec), f.risk.on_venue_reject(2 * kSec), f.risk.on_venue_reject(3 * kSec);
        EXPECT_FALSE(f.risk.killed());  // 3 in 10 s is the limit
        f.risk.on_venue_reject(20 * kSec);  // window moved on
        EXPECT_FALSE(f.risk.killed());
        f.risk.on_venue_reject(21 * kSec), f.risk.on_venue_reject(22 * kSec), f.risk.on_venue_reject(23 * kSec);
        EXPECT_EQ(f.risk.kill_reason(), KillReason::RejectSpike);
    }
}
