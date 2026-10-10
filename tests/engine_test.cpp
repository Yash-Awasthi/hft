#include "pm/engine.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <fstream>
#include <string>
#include <vector>

#include "pm/reader.hpp"
#include "temp_dir.hpp"

using namespace hft::pm;

namespace {

constexpr std::int64_t kS = 1'000'000'000;

std::string book(const char* id, const char* bid, const char* ask) {
    return std::string(R"({"event_type":"book","asset_id":")") + id + R"(","bids":[{"price":")" + bid +
           R"(","size":"500"}],"asks":[{"price":")" + ask + R"(","size":"500"}],"tick_size":"0.01"})";
}
std::string change(const char* id, const char* side, const char* px, const char* size) {
    return std::string(R"({"event_type":"price_change","price_changes":[{"asset_id":")") + id + R"(","price":")" + px +
           R"(","size":")" + size + R"(","side":")" + side + R"("}]})";
}
std::string trade(const char* id, const char* side, const char* px, const char* size) {
    return std::string(R"({"event_type":"last_trade_price","asset_id":")") + id + R"(","price":")" + px +
           R"(","size":")" + size + R"(","side":")" + side + R"("})";
}
std::string control(const char* type, int conn) {
    return std::string(R"({"event_type":")") + type + R"(","conn":)" + std::to_string(conn) + "}";
}

struct Line {
    std::int64_t ns;
    std::string json;
};

EngineParams params() {
    EngineParams p;
    p.maker.warmup_s = 0;
    p.maker.requote_s = 0;
    return p;
}

void feed(Engine& e, const std::vector<Line>& lines) {
    Decoder d;
    for (const Line& l : lines) ASSERT_TRUE(d.decode(l.json, [&](const Event& ev) { e.on_event(l.ns, ev); }));
}

std::vector<std::string> describe(const std::vector<Decision>& v) {
    std::vector<std::string> out;
    for (const Decision& d : v)
        out.push_back(std::to_string(d.kind) + " " + std::to_string(d.ns) + " " + std::to_string(d.token) + " " +
                      std::to_string(d.bid) + " " + std::to_string(d.ask) + " " + std::to_string(d.inventory));
    return out;
}

std::vector<Line> session() {
    std::vector<Line> l;
    std::int64_t t = kS;
    l.push_back({t, control("_heartbeat", 0)});
    l.push_back({t += 1000, book("A", "0.48", "0.52")});
    l.push_back({t += 1000, book("B", "0.30", "0.33")});
    for (int i = 0; i < 200; ++i) {
        l.push_back({t += 7'000'000, change("A", i % 2 ? "BUY" : "SELL", i % 2 ? "0.47" : "0.53", std::to_string(10 + i).c_str())});
        if (i % 9 == 0) l.push_back({t += 3'000'000, trade("A", i % 18 ? "BUY" : "SELL", i % 18 ? "0.6" : "0.4", "50")});
        if (i % 50 == 0) l.push_back({t += 1, control("_heartbeat", 0)});
    }
    return l;
}

}  // namespace

TEST(Engine, SameInputSameDecisionsAlsoThroughARecording) {
    const std::vector<Line> lines = session();
    Engine a(params()), b(params());
    for (Engine* e : {&a, &b}) e->set_conn(e->token("A"), 0), e->set_conn(e->token("B"), 0);
    feed(a, lines);
    feed(b, lines);
    const auto da = describe(a.decisions());
    EXPECT_GT(a.fills(), 0u);
    EXPECT_GT(da.size(), 20u);
    EXPECT_EQ(da, describe(b.decisions()));

    TempDir dir("engine_replay");
    std::filesystem::create_directories(dir.path / "c0");
    std::string text;
    for (const Line& l : lines) text += std::to_string(l.ns) + " " + l.json + "\n";
    std::string z(ZSTD_compressBound(text.size()), '\0');
    z.resize(ZSTD_compress(z.data(), z.size(), text.data(), text.size(), 1));
    std::ofstream(dir.path / "c0" / "h.jsonl.zst", std::ios::binary) << z;
    Engine c(params());
    c.set_conn(c.token("A"), 0), c.set_conn(c.token("B"), 0);
    read_records(dir.path, [&](std::int64_t ns, const Event& e) { c.on_event(ns, e); });
    EXPECT_EQ(da, describe(c.decisions()));
}

TEST(Engine, LossStopHaltsQuotingForTheSession) {
    auto p = params();
    p.loss_stop_usd = 1;
    Engine e(p);
    feed(e, {{kS, book("A", "0.48", "0.52")},
             {kS + 1, trade("A", "SELL", "0.01", "1")},     // trades through our bid: we buy 100
             {kS + 2, book("A", "0.10", "0.12")},           // the mid falls: about -$38 marked
             {kS + 1 * kS, change("A", "BUY", "0.10", "1")}});
    EXPECT_TRUE(e.halted());
    EXPECT_LT(e.maker(e.token("A")).bid_px(), 0);
    const auto& d = e.decisions();
    EXPECT_TRUE(std::any_of(d.begin(), d.end(), [](const Decision& x) { return x.kind == Decision::Halt; }));
}

TEST(Engine, StaleConnectionPausesItsTokensUntilHeardFromAgain) {
    Engine e(params());
    e.set_conn(e.token("A"), 0);
    e.set_conn(e.token("B"), 1);
    feed(e, {{kS, control("_heartbeat", 0)},
             {kS, control("_heartbeat", 1)},
             {kS + 1, book("A", "0.48", "0.52")},
             {kS + 2, book("B", "0.48", "0.52")}});
    EXPECT_GE(e.maker(e.token("A")).bid_px(), 0);
    // Connection 1 keeps its heartbeat; connection 0 is silent for 25 s.
    feed(e, {{10 * kS, control("_heartbeat", 1)}, {20 * kS, control("_heartbeat", 1)}, {26 * kS, control("_heartbeat", 1)}});
    EXPECT_FALSE(e.maker(e.token("A")).enabled());
    EXPECT_TRUE(e.maker(e.token("B")).enabled());
    feed(e, {{27 * kS, control("_heartbeat", 0)}, {28 * kS, control("_heartbeat", 1)}});
    EXPECT_TRUE(e.maker(e.token("A")).enabled());
}

TEST(Engine, ReconnectInvalidatesBooksUntilTheNextSnapshot) {
    Engine e(params());
    e.set_conn(e.token("A"), 0);
    feed(e, {{kS, book("A", "0.48", "0.52")}, {kS + 1, control("_reconnect", 0)}, {kS + 2, change("A", "BUY", "0.49", "5")}});
    EXPECT_FALSE(e.seeded(e.token("A")));
    EXPECT_EQ(e.maker(e.token("A")).book.levels(), 0u);
    EXPECT_EQ(e.reconnects(), 1u);
    feed(e, {{kS + 3, book("A", "0.40", "0.45")}});
    EXPECT_EQ(e.maker(e.token("A")).book.best_bid(), 4000);
}

TEST(Engine, GrossInventoryCapPausesEveryToken) {
    auto p = params();
    p.max_gross_shares = 50;
    Engine e(p);
    feed(e, {{kS, book("A", "0.48", "0.52")},
             {kS, book("B", "0.48", "0.52")},
             {kS + 1, trade("A", "SELL", "0.01", "1")},  // we buy 100 of A
             {2 * kS, change("B", "BUY", "0.47", "1")}});
    EXPECT_FALSE(e.maker(e.token("A")).enabled());
    EXPECT_FALSE(e.maker(e.token("B")).enabled());
    EXPECT_FALSE(e.halted());
}

TEST(Engine, VenueMakerInventoryFollowsTheLedgerThroughSettlementFailure) {
    auto p = params();
    p.venue = true;
    p.sim.compat_side = true;
    p.sim.settle_delay = kS;
    p.sim.p_settle_fail_ppm = 1000000;
    Engine e(p);
    feed(e, {{kS, book("A", "0.48", "0.52")}, {kS + 1, trade("A", "SELL", "0.01", "1")}});  // we buy 100 of A
    const std::uint32_t a = e.token("A");
    ASSERT_EQ(e.ledger().pos(a), 100'000'000);
    EXPECT_EQ(e.maker(a).inventory(), 100);
    feed(e, {{3 * kS, change("A", "BUY", "0.47", "1")}});  // past the settlement delay: it failed
    EXPECT_EQ(e.ledger().pos(a), 0);
    EXPECT_EQ(e.maker(a).inventory(), 0);
    EXPECT_EQ(e.maker(a).pnl(), 0);  // the cash came back too
}

TEST(Engine, VenueMakerAsksOnlyUpToHeldShares) {
    auto p = params();
    p.venue = true;
    Engine e(p);
    feed(e, {{kS, book("A", "0.48", "0.52")}});
    const std::uint32_t a = e.token("A");
    EXPECT_GE(e.maker(a).ask_px(), 0);
    EXPECT_EQ(e.oms().open_orders(), 1u);  // the bid; no shares, so no ask
    feed(e, {{kS + 1, trade("A", "SELL", "0.48", "530.505")}});  // 500 ahead of our bid, then we buy 30.505
    ASSERT_EQ(e.ledger().pos(a), 30'505'000);
    EXPECT_EQ(e.ledger().available_pos(a), 5'000);  // 30.50 offered: sizes have 2 dp
    EXPECT_EQ(e.oms().open_orders(), 2u);
}

TEST(Engine, VenueRejectSpikeTripsTheKillSwitch) {
    auto p = params();
    p.venue = true;
    p.sim.lat_in = p.sim.lat_out = 10'000'000;
    p.risk.reject_spike = 0;
    Engine e(p);
    // Our bid at 0.48 is in flight when the ask comes down to 0.48: post-only, so the venue rejects it.
    feed(e, {{kS, book("A", "0.48", "0.52")}, {kS + 1'000'000, change("A", "SELL", "0.48", "100")},
             {kS + 200'000'000, control("_heartbeat", 0)}});
    EXPECT_EQ(e.venue_stats().rejects, 1u);
    EXPECT_EQ(e.exec_risk().kill_reason(), hft::exec::KillReason::RejectSpike);
}

TEST(Engine, VenueLossBeyondTheDailyStopTripsTheKillSwitch) {
    auto p = params();
    p.venue = true;
    p.risk.daily_stop = 10 * hft::exec::kDollar;
    Engine e(p);
    feed(e, {{kS, book("A", "0.48", "0.52")}, {kS + 1, trade("A", "SELL", "0.48", "600")}});  // we buy 100 at 0.48
    ASSERT_EQ(e.ledger().pos(e.token("A")), 100'000'000);
    feed(e, {{2 * kS, change("A", "BUY", "0.48", "0")}, {2 * kS, change("A", "BUY", "0.40", "500")},
             {3 * kS, control("_heartbeat", 0)}});
    EXPECT_FALSE(e.exec_risk().killed());  // marked at the 0.40 bid: 8 USD down
    feed(e, {{4 * kS, change("A", "BUY", "0.40", "0")}, {4 * kS, change("A", "BUY", "0.37", "500")},
             {5 * kS, control("_heartbeat", 0)}});
    EXPECT_EQ(e.exec_risk().kill_reason(), hft::exec::KillReason::Loss);  // 11 USD down
    EXPECT_GT(e.ledger().capital_usd_days(), 0);
}
