#include "exec/types.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

using namespace hft::exec;

TEST(ExecTypes, NotionalIsExact) {
    EXPECT_EQ(notional(1, 1), 1);  // 0.0001 USD/share * 0.000001 share = 1e-10 USD
    EXPECT_EQ(notional(kPxOne, kShare), kDollar);
    EXPECT_EQ(notional(3660, 12'500'000), 45'750'000'000);  // 0.366 * 12.5 = 4.575 USD
}

TEST(ExecTypes, TakerFeeFollowsTheScheduleAndRoundsToFiveDecimals) {
    hft::pm::MarketRules r;
    r.fee_rate_ppm = 50'000;
    r.fee_exp = 1;
    EXPECT_EQ(taker_fee(100 * kShare, 5000, r), 125 * kDollar / 100);  // 100 * 0.05 * 0.25 = 1.25
    r.fee_rate_ppm = 70'000;
    EXPECT_EQ(taker_fee(3 * kShare, 1230, r), 2265 * kDollar / 100'000);  // 0.0226533 -> 0.02265
    r.fee_rate_ppm = 10'000;
    EXPECT_EQ(taker_fee(kShare, 100, r), kDollar / 100'000 * 10);  // 0.000099 -> 0.00010
    EXPECT_EQ(taker_fee(kShare / 100, 100, r), 0);                 // 0.00000099 -> 0
    r.fee_rate_ppm = 72'000;
    r.fee_exp = 2;
    EXPECT_EQ(taker_fee(10 * kShare, 5000, r), 45 * kDollar / 1000);  // 10 * 0.072 * 0.0625 = 0.045
    r.fees = false;
    EXPECT_EQ(taker_fee(10 * kShare, 5000, r), 0);
}

RC_GTEST_PROP(ExecTypes, TakerFeeIsTheNearestFiveDecimalAmount, ()) {
    hft::pm::MarketRules r;
    r.fee_rate_ppm = *rc::gen::inRange<std::uint32_t>(0, 100'001);
    r.fee_exp = *rc::gen::inRange<std::uint8_t>(0, 3);
    const Qty q = *rc::gen::inRange<Qty>(1, 1'000'000 * kShare);
    const Px p = *rc::gen::inRange<Px>(1, kPxOne);
    const Usd f = taker_fee(q, p, r);
    constexpr Usd step = kDollar / 100'000;
    RC_ASSERT(f % step == 0);
    // Exact fee in 1e-10 USD is num / den; |f - num/den| <= step / 2.
    __int128 num = static_cast<__int128>(q) * r.fee_rate_ppm, den = 100;
    for (int e = 0; e < r.fee_exp; ++e) num *= static_cast<__int128>(p) * (kPxOne - p), den *= 100'000'000;
    const __int128 diff = static_cast<__int128>(f) * den - num;
    RC_ASSERT(2 * (diff < 0 ? -diff : diff) <= den * step);
}

TEST(ExecTypes, ClientIdsCarryTheSessionAndNeverRepeat) {
    ClientIds ids(7);
    const std::uint64_t a = ids.next(), b = ids.next();
    EXPECT_NE(a, b);
    EXPECT_EQ(session_of(a), 7u);
    EXPECT_EQ(seq_of(b), seq_of(a) + 1);
}
