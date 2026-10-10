#include "pm/config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace hft;
using namespace hft::pm;

namespace {

EngineParams parse(const char* text) {
    EngineParams p;
    apply_config(p, text, "test");
    return p;
}

}  // namespace

TEST(Config, EmptyKeepsTheDefaults) {
    const EngineParams p = parse("# nothing\n\n   \n");
    EXPECT_EQ(p.capital, 1'000'000 * exec::kDollar);
    EXPECT_EQ(p.risk.daily_stop, 20'000 * exec::kDollar);
    EXPECT_TRUE(p.long_only);
}

TEST(Config, KeysSetTheirFieldsInTheirUnits) {
    const EngineParams p = parse(
        "seed = 7\n"
        "lat_ms = 91  # half the measured round trip\n"
        "jitter_ms=20\n"
        "settle_ms = 1500\n"
        "p_settle_fail = 0.2\n"
        "disconnect_every_s = 120\n"
        "disconnect_for_s = 8\n"
        "capital_usd = 250000\n"
        "token_cap_usd = 12500.5\n"
        "max_open = 64\n"
        "size = 10\n"
        "gamma = 0.01\n"
        "arb = 1\n"
        "arb_min_edge = 0.005\n"
        "attempt_bound_usd = 1000\n"
        "merge_ms = 3000\n");
    EXPECT_EQ(p.sim.seed, 7u);
    EXPECT_EQ(p.sim.lat_in, 91'000'000);
    EXPECT_EQ(p.sim.lat_out, 91'000'000);
    EXPECT_EQ(p.sim.jitter, 20'000'000);
    EXPECT_EQ(p.sim.settle_delay, 1'500'000'000);
    EXPECT_EQ(p.sim.p_settle_fail_ppm, 200'000u);
    EXPECT_EQ(p.sim.disc_every, 120'000'000'000);
    EXPECT_EQ(p.sim.disc_for, 8'000'000'000);
    EXPECT_EQ(p.capital, 250'000 * exec::kDollar);
    EXPECT_EQ(p.risk.token_cap, 125'005 * exec::kDollar / 10);
    EXPECT_EQ(p.risk.max_open, 64u);
    EXPECT_EQ(p.maker.size, 10);
    EXPECT_EQ(p.maker.gamma, 0.01);
    EXPECT_TRUE(p.arb.on);
    EXPECT_EQ(p.arb.min_edge, 50);
    EXPECT_EQ(p.arb.attempt_bound, 1'000 * exec::kDollar);
    EXPECT_EQ(p.arb.merge_delay, 3'000'000'000);
}

TEST(Config, CompatModeLiftsTheLimitsTheOldModelNeverHad) {
    const EngineParams p = parse("compat = 1\n");
    EXPECT_TRUE(p.sim.compat_side);
    EXPECT_FALSE(p.long_only);
    EXPECT_TRUE(p.risk.allow_short);
    EXPECT_EQ(p.risk.reject_window, 0);
    EXPECT_GT(p.risk.token_cap, 1'000'000 * exec::kDollar);
}

TEST(Config, RefusesAnythingNotExactlyRight) {
    for (const char* bad : {
             "capitl_usd = 5\n",          // unknown key
             "capital_usd = 1e12\n",      // above the hard bound
             "lat_ms = -1\n",             // below it
             "p_dup = 1.5\n",             // a probability above 1
             "seed = 5x\n",               // trailing garbage
             "seed = \n",                 // no value
             "seed 5\n",                  // no '='
             "seed = 1.5\n",              // integer key
             "max_open = 0\n",            // zero open orders allowed is a typo, not a setting
             "seed = 1\nseed = 2\n",      // a duplicate hides which value was meant
             "gamma = nan\n",
             "gamma = inf\n",
         }) {
        EXPECT_THROW(parse(bad), std::runtime_error) << bad;
    }
}

TEST(Config, ErrorsNameTheOriginAndLine) {
    EngineParams p;
    try {
        apply_config(p, "seed = 1\nbogus = 2\n", "run/exec.cfg");
        FAIL();
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "run/exec.cfg:2: unknown key bogus");
    }
}

TEST(Config, SeparateCallsMayOverrideEarlierOnes) {
    EngineParams p;
    apply_config(p, "seed = 1\n", "file");
    apply_config(p, "seed = 2", "--set");
    EXPECT_EQ(p.sim.seed, 2u);
}
