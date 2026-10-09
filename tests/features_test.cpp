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
    book::TickBook<> b;
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
    book::TickBook<> b;
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
    book::TickBook<> b;
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

// Leakage check of DESIGN.md section 1: features up to t are bit-identical whatever happens
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

#include "strategy/labeler.hpp"

TEST(Labeler, EventAndClockHorizonsAgainstBruteForce) {
    std::vector<std::uint64_t> ts;
    std::vector<double> mid;
    const rng::Stream g(3, 0, 0);
    std::uint64_t t = 1000;
    for (std::uint32_t i = 0; i < 3000; ++i) {
        t += g.draw(i, 0)[0] % 5'000'000;  // ties allowed when the draw is 0 mod 5e6
        ts.push_back(t);
        mid.push_back(i % 97 == 0 ? std::nan("") : 100 + (g.draw(i, 1)[0] % 21) * 0.5);
    }
    const Horizons h{{1, 10, 100}, {1'000'000, 50'000'000, 1'000'000'000}};
    std::vector<double> y;
    label(ts, mid, h, y);
    const std::size_t w = h.count();
    for (std::size_t i = 0; i < ts.size(); ++i) {
        for (std::size_t c = 0; c < h.events.size(); ++c) {
            const double want =
                i + h.events[c] < ts.size() ? mid[i + h.events[c]] - mid[i] : std::nan("");
            const double got = y[i * w + c];
            EXPECT_TRUE(got == want || (std::isnan(got) && std::isnan(want))) << i << " " << c;
        }
        for (std::size_t c = 0; c < h.clock_ns.size(); ++c) {
            double want = std::nan("");
            if (ts[i] + h.clock_ns[c] <= ts.back()) {
                std::size_t j = i;
                for (std::size_t k = i; k < ts.size() && ts[k] <= ts[i] + h.clock_ns[c]; ++k) j = k;
                want = mid[j] - mid[i];
            }
            const double got = y[i * w + h.events.size() + c];
            EXPECT_TRUE(got == want || (std::isnan(got) && std::isnan(want))) << i << " " << c;
        }
    }
}

#include "strategy/lifecycles.hpp"

TEST(Lifecycles, OutcomesQueueAndMarkouts) {
    LifecycleTracker t(1, 60.0, {0.001, 1.0});
    ItchWriter m;
    std::uint64_t seq = 0;
    auto on = [&](const ItchWriter& w) { t.step(w.b.data(), w.b.size(), ++seq); };
    const std::uint64_t t0 = m.ts;
    on(m.add(1, 'B', 100, 10'0000));
    on(m.add(2, 'S', 100, 10'0100));
    m.ts = t0 + 1'000'000;
    on(m.exec(1, 40));                // first fill of 1 after 1 ms
    on(m.add(3, 'B', 100, 10'0000));  // joins behind 60 shares
    on(m.add(5, 'S', 100, 10'0100));  // joins behind 100 shares
    m.ts = t0 + 3'000'000;
    on(m.add(6, 'S', 100, 10'0050));  // better ask: 2 and 5 move away
    m.ts = t0 + 4'000'000;
    on(m.del(3));  // own cancel: censored
    m.ts = t0 + 2'000'000'000;
    on(m.add(7, 'B', 1, 9'0000));  // a later event so markouts resolve
    t.finish();

    std::map<std::uint64_t, Lifecycle> by;
    for (const auto& l : t.lifecycles()) by[l.ref] = l;
    ASSERT_EQ(by.count(1) + by.count(2) + by.count(3) + by.count(5) + by.count(6), 5u);
    EXPECT_EQ(by[1].outcome, Outcome::Fill);
    EXPECT_NEAR(by[1].duration_s, 0.001, 1e-12);
    EXPECT_EQ(by[2].outcome, Outcome::Away);
    EXPECT_NEAR(by[2].duration_s, 0.003, 1e-12);
    EXPECT_EQ(by[5].outcome, Outcome::Away);
    EXPECT_DOUBLE_EQ(by[5].queue_ahead, 100);
    EXPECT_EQ(by[3].outcome, Outcome::Censored);
    EXPECT_DOUBLE_EQ(by[3].queue_ahead, 60);
    EXPECT_NEAR(by[3].duration_s, 0.003, 1e-12);
    EXPECT_FALSE(by.count(7));  // not at the best
    ASSERT_EQ(t.fills().size(), 1u);
    const FillMark& f = t.fills()[0];
    EXPECT_EQ(f.side, 1);
    // Mid 1000.5 ticks after the fill, 1000.25 a second later (ask improved to 10.005).
    EXPECT_DOUBLE_EQ(f.markout[0], 1000.5 - 1000);
    EXPECT_DOUBLE_EQ(f.markout[1], 1000.25 - 1000);
}
