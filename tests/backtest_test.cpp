#include <gtest/gtest.h>
#include <zstd.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "backtest/strategies.hpp"
#include "data/gzip.hpp"
#include "data/store.hpp"
#include "temp_dir.hpp"

using namespace hft;
using namespace hft::backtest;

namespace {

// The committed fixture as a store. Its clock runs from 04:00 for about 20 s, so the window
// quotes for 12 s and then flattens.
struct FixtureStore {
    TempDir dir{"bt_store"};
    FixtureStore() {
        std::ifstream f(HFT_SOURCE_DIR "/tests/data/flow-7-4-20k.itch.zst", std::ios::binary);
        const std::vector<std::uint8_t> comp((std::istreambuf_iterator<char>(f)), {});
        std::vector<std::uint8_t> raw(ZSTD_getFrameContentSize(comp.data(), comp.size()));
        ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
        data::StoreWriter w(dir.path, 1 << 24, 1 << 20);
        itch::Frame fr{};
        std::uint64_t seq = 0;
        for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, fr));
             pos += k)
            w.append(load_be16(fr.data + 1), ++seq, itch::detail::read_header(fr.data).timestamp,
                     fr.data, fr.size);
        w.finish();
    }
};

Config window() {
    Config c;
    c.start_ns = 4ull * 3600 * 1'000'000'000;
    c.stop_ns = c.start_ns + 12'000'000'000ull;
    c.end_ns = c.start_ns + 25'000'000'000ull;
    c.exchange.maker_rebate = 2000;
    c.exchange.taker_fee = 3000;
    c.market_data_ns = 50'000;
    c.order_entry_ns = 20'000;
    c.risk.collar_ticks = 1000;
    return c;
}

}  // namespace

TEST(Backtest, ZeroStrategyHasZeroPnl) {
    FixtureStore fx;
    Zero z;
    Backtest<Zero> bt(window(), z);
    const Summary s = bt.run(fx.dir.path.string(), 1, {3});
    EXPECT_EQ(s.total, 0);
    EXPECT_EQ(s.fills, 0u);
    EXPECT_EQ(s.orders, 0u);
    EXPECT_GT(s.events, 1000u);
}

TEST(Backtest, NaiveJoinTradesKeepsTheIdentityAndFlattens) {
    FixtureStore fx;
    NaiveJoin n;
    Backtest<NaiveJoin> bt(window(), n);
    const Summary s = bt.run(fx.dir.path.string(), 1, {3});
    EXPECT_GT(s.fills, 10u);
    EXPECT_EQ(s.total, s.spread + s.inventory_pnl + s.fees);
    EXPECT_LE(s.max_abs_inventory, 600);
    EXPECT_EQ(s.end_inventory, 0);
}

TEST(Backtest, RandomTakingLosesAboutHalfTheSpreadPlusFees) {
    FixtureStore fx;
    RandomTaker r;
    r.rate = 0.02;
    Backtest<RandomTaker> bt(window(), r);
    const Summary s = bt.run(fx.dir.path.string(), 1, {3});
    ASSERT_GT(s.volume, 0);
    EXPECT_LT(s.spread, 0);  // every take pays half the spread
    EXPECT_LT(s.fees, 0);
}
