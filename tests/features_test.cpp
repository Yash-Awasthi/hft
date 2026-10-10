#include "strategy/features.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "core/endian.hpp"
#include "core/philox.hpp"
#include "itch_writer.hpp"
#include "sources/random_flow.hpp"
#include "strategy/market_feed.hpp"

using namespace hft;
using namespace hft::strategy;

namespace {

struct Rig {
    book::TickBook b;
    MarketFeed<> feed{b};
    SymbolFeatures f;
    std::uint64_t seq = 0;
    bool on(const ItchWriter& m) {
        MarketEvent e;
        if (!feed.on_itch(m.b.data(), m.b.size(), ++seq, e)) return false;
        f.on_event(b, e);
        return true;
    }
    double v(const char* name) const {
        for (std::size_t i = 0; i < SymbolFeatures::kCount; ++i)
            if (std::strcmp(SymbolFeatures::kNames[i], name) == 0) return f.values()[i];
        ADD_FAILURE() << "no feature " << name;
        return 0;
    }
};

const double kW = 1 - std::exp2(-1.0 / 50);

}  // namespace

TEST(Features, BookStateAndOrderFlowImbalance) {
    Rig r;
    ItchWriter m;
    r.on(m.add(1, 'B', 100, 10'0000));
    EXPECT_DOUBLE_EQ(r.v("ofi_l1"), kW * 100);
    EXPECT_TRUE(std::isnan(r.v("spread_ticks")));
    const double ofi1 = kW * 100;
    r.on(m.add(2, 'S', 200, 10'0100));
    EXPECT_DOUBLE_EQ(r.v("ofi_l1"), ofi1 + kW * (-200 - ofi1));
    EXPECT_DOUBLE_EQ(r.v("spread_ticks"), 1);
    EXPECT_DOUBLE_EQ(r.v("imbalance_l1"), -1.0 / 3);
    EXPECT_DOUBLE_EQ(r.f.mid(), 1000.5);
    r.on(m.add(3, 'B', 50, 9'9900));
    r.on(m.add(4, 'S', 250, 10'0200));
    EXPECT_DOUBLE_EQ(r.v("imbalance_l3"), (150.0 - 450) / 600);
}

TEST(Features, TradesHiddenFlowAndSpreadClock) {
    Rig r;
    ItchWriter m;
    r.on(m.add(1, 'B', 100, 10'0000));
    r.on(m.add(2, 'S', 100, 10'0100));
    EXPECT_DOUBLE_EQ(r.v("log_since_spread"), 0);
    m.ts += 1'000'000'000;
    r.on(m.exec(1, 40));  // a resting bid is hit: seller-initiated
    EXPECT_DOUBLE_EQ(r.v("trade_sign"), -kW);
    EXPECT_DOUBLE_EQ(r.v("aggressor_imb"), -1);
    EXPECT_DOUBLE_EQ(r.v("log_since_spread"), std::log1p(1.0));
    EXPECT_DOUBLE_EQ(r.v("log_last_trade"), std::log1p(40.0));
    EXPECT_LT(r.v("hawkes_imbalance"), 0);
    // P messages always say 'B'; the aggressor comes from the print against the mid.
    r.on(m.hidden('B', 60, 10'0080));  // above the mid of 10.005: buyer-initiated
    EXPECT_DOUBLE_EQ(r.v("hidden_imb"), 1);
    EXPECT_DOUBLE_EQ(r.v("aggressor_imb"), (60.0 - 40) / 100);
    r.on(m.hidden('B', 30, 10'0050));  // at the mid: unsigned
    EXPECT_DOUBLE_EQ(r.v("hidden_imb"), 60.0 / 90);
    EXPECT_FALSE(r.on(m.cancel(99, 1)));  // unknown order: no event, no book change
}

namespace {

std::vector<std::vector<std::uint8_t>> symbol_messages(const std::vector<std::uint8_t>& raw,
                                                       std::uint16_t locate) {
    std::vector<std::vector<std::uint8_t>> out;
    itch::Frame f{};
    for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, f));
         pos += k)
        if (load_be16(f.data + 1) == locate) out.emplace_back(f.data, f.data + f.size);
    return out;
}

// Feature rows (raw bytes) after each of the first `upto` messages.
std::vector<std::uint8_t> rows(const std::vector<std::vector<std::uint8_t>>& msgs,
                               std::size_t upto) {
    book::TickBook b;
    MarketFeed<> feed(b);
    SymbolFeatures f;
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        MarketEvent e;
        if (feed.on_itch(msgs[i].data(), msgs[i].size(), i, e)) f.on_event(b, e);
        if (i < upto) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(f.values().data());
            out.insert(out.end(), p, p + sizeof(SymbolFeatures::Values));
        }
    }
    return out;
}

}  // namespace

// Feature rows after every message that reaches the engine, with or without the incremental
// depth walk.
std::vector<std::uint8_t> all_rows(const std::vector<std::vector<std::uint8_t>>& msgs,
                                   bool incremental) {
    book::TickBook b;
    MarketFeed<> feed(b);
    FeatureParams p;
    p.incremental_depth = incremental;
    SymbolFeatures f(p);
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        MarketEvent e;
        if (!feed.on_itch(msgs[i].data(), msgs[i].size(), i, e)) continue;
        f.on_event(b, e);
        const auto* q = reinterpret_cast<const std::uint8_t*>(f.values().data());
        out.insert(out.end(), q, q + sizeof(SymbolFeatures::Values));
    }
    return out;
}

TEST(Features, IncrementalDepthEqualsTheFullWalkBitForBit) {
    std::size_t compared = 0;
    for (std::uint64_t seed : {7, 21, 99}) {
        std::vector<std::uint8_t> raw;
        sources::RandomFlow(seed, 4).day(20000, raw);
        for (std::uint16_t locate = 0; locate <= 4; ++locate) {
            const auto msgs = symbol_messages(raw, locate);
            const auto full = all_rows(msgs, false);
            EXPECT_EQ(all_rows(msgs, true), full) << "seed " << seed << " locate " << locate;
            compared += full.size() / sizeof(SymbolFeatures::Values);
        }
    }
    EXPECT_GT(compared, 10000u);
}

// Leakage check of DESIGN.md (research-archive tag) section 1: features up to t are bit-identical whatever happens
// after t (events deleted, shuffled, or replaced by unrelated synthetic flow).
TEST(Features, UnchangedWhenTheFutureIsPerturbed) {
    std::vector<std::uint8_t> raw, other;
    sources::RandomFlow(7, 4).day(20000, raw);
    sources::RandomFlow(99, 4).day(20000, other);
    const auto msgs = symbol_messages(raw, 1);
    const auto alien = symbol_messages(other, 1);
    for (std::size_t t : {msgs.size() / 4, msgs.size() / 2, msgs.size() - 10}) {
        const auto want = rows(msgs, t);
        auto cut = msgs;
        cut.resize(t);
        EXPECT_EQ(rows(cut, t), want) << "deleted after " << t;
        auto shuffled = msgs;
        const rng::Stream g(5, 0, 0);
        for (std::size_t i = shuffled.size() - 1; i > t; --i)
            std::swap(shuffled[i],
                      shuffled[t + g.draw(static_cast<std::uint32_t>(i), 0)[0] % (i - t + 1)]);
        EXPECT_EQ(rows(shuffled, t), want) << "shuffled after " << t;
        auto replaced = msgs;
        replaced.resize(t);
        replaced.insert(replaced.end(), alien.begin(), alien.end());
        EXPECT_EQ(rows(replaced, t), want) << "replaced after " << t;
    }
}
