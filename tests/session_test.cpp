#include "pm/session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "pm/reader.hpp"
#include "temp_dir.hpp"

using namespace hft::pm;

TEST(Session, SavesAndLoadsTokensAndGroups) {
    TempDir dir("pm_session");
    Session s;
    s.tokens = {{"111", "Team A\tYes", 0, {}}, {"222", "Team A No", 0, {}}, {"333", "Team B Yes", 1, {}}};
    s.groups = {{"event-x", {"111", "333"}}, {"market-a", {"111", "222"}}};
    s.save(dir.path / "session.tsv");
    const Session t = Session::load(dir.path / "session.tsv");
    ASSERT_EQ(t.tokens.size(), 3u);
    EXPECT_EQ(t.tokens[0].label, "Team A Yes");  // the tab became a space
    EXPECT_EQ(t.tokens[2].conn, 1);
    ASSERT_EQ(t.groups.size(), 2u);
    EXPECT_EQ(t.groups[0].ids, (std::vector<std::string>{"111", "333"}));

    Engine e{EngineParams{}};
    t.apply(e);
    EXPECT_EQ(e.tokens(), 3u);
    EXPECT_EQ(e.arb().stats().size(), 2u);
}

TEST(Gamma, ReadsEachMarketsTradingRules) {
    // Fields as the metadata API sends them (2026-10-10).
    const auto m = hft::net::parse_json(
        R"({"slug":"a","conditionId":"0x1","clobTokenIds":"[\"11\", \"12\"]","outcomes":"[\"Yes\", \"No\"]",)"
        R"("active":true,"closed":false,"enableOrderBook":true,"acceptingOrders":true,"orderMinSize":5,)"
        R"("orderPriceMinTickSize":0.001,"negRisk":true,"feesEnabled":true,"secondsDelay":1,)"
        R"("feeSchedule":{"exponent":1,"rate":0.05,"takerOnly":true,"rebateRate":0.15}})");
    const Market mk = hft::pm::market_from(m, "t");
    EXPECT_EQ(mk.rules.tick, 10);  // 0.001 in 1e-4 price units
    EXPECT_EQ(mk.rules.min_qty, 5'000'000);
    EXPECT_TRUE(mk.rules.neg_risk);
    EXPECT_TRUE(mk.rules.fees);
    EXPECT_EQ(mk.rules.fee_rate_ppm, 50'000u);
    EXPECT_EQ(mk.rules.fee_exp, 1);
    EXPECT_EQ(mk.rules.delay_ms, 1000);

    const auto free = hft::net::parse_json(
        R"({"clobTokenIds":"[\"13\"]","orderMinSize":5,"orderPriceMinTickSize":0.01,"feesEnabled":false,"feeSchedule":null,"secondsDelay":null})");
    const Market f = hft::pm::market_from(free, "t");
    EXPECT_EQ(f.rules.tick, 100);
    EXPECT_FALSE(f.rules.fees);
    EXPECT_EQ(f.rules.delay_ms, 0);
}

TEST(Session, KeepsMarketRulesAndReadsOlderFilesWithDefaults) {
    TempDir dir("pm_session_rules");
    Session s;
    s.tokens = {{"111", "A Yes", 0, {}}};
    s.tokens[0].rules = {10, 2'500'000, true, true, 40'000, 1, 1000};
    s.save(dir.path / "session.tsv");
    const Session t = Session::load(dir.path / "session.tsv");
    ASSERT_EQ(t.tokens.size(), 1u);
    const auto& r = t.tokens[0].rules;
    EXPECT_EQ(t.tokens[0].label, "A Yes");
    EXPECT_EQ(r.tick, 10);
    EXPECT_EQ(r.min_qty, 2'500'000);
    EXPECT_TRUE(r.neg_risk);
    EXPECT_EQ(r.fee_rate_ppm, 40'000u);
    EXPECT_EQ(r.delay_ms, 1000);

    std::ofstream(dir.path / "old.tsv") << "token\t222\t1\tB No\n";
    const Session o = Session::load(dir.path / "old.tsv");
    ASSERT_EQ(o.tokens.size(), 1u);
    EXPECT_EQ(o.tokens[0].label, "B No");
    EXPECT_EQ(o.tokens[0].rules.min_qty, hft::pm::MarketRules{}.min_qty);
    EXPECT_EQ(o.tokens[0].rules.tick, 0);  // unknown: taken from the book
}

TEST(Recorder, RotatesHourlyAndReadsBackInOrder) {
    TempDir dir("pm_recorder");
    constexpr std::int64_t kHour = 3'600'000'000'000;
    const std::int64_t t0 = 492'000 * kHour + kHour - 2'000'000'000;  // two seconds before an hour ends
    std::vector<std::pair<std::int64_t, std::string>> in;
    for (int i = 0; i < 4000; ++i)
        in.emplace_back(t0 + std::int64_t{i} * 1'000'000,
                        R"({"event_type":"last_trade_price","asset_id":"7","price":"0.5","size":")" +
                            std::to_string(i + 1) + R"(","side":"BUY"})");
    {
        Recorder r(dir.path, 1.0);
        for (const auto& [ns, text] : in) r.write(ns, text);
    }
    std::vector<std::string> files;
    for (const auto& f : std::filesystem::directory_iterator(dir.path / "c0")) files.push_back(f.path().filename().string());
    std::sort(files.begin(), files.end());
    ASSERT_EQ(files.size(), 2u);  // 4 s of records across the hour boundary
    EXPECT_TRUE(files[0].ends_with(".jsonl.zst"));

    std::vector<std::int64_t> ns;
    double shares = 0;
    read_records(dir.path, [&](std::int64_t t, const Event& e) { ns.push_back(t), shares += e.size; });
    ASSERT_EQ(ns.size(), in.size());
    for (std::size_t i = 0; i < ns.size(); ++i) ASSERT_EQ(ns[i], in[i].first) << i;
    EXPECT_EQ(shares, 4000.0 * 4001 / 2);
}

TEST(Recorder, StopsWritingAtTheCap) {
    TempDir dir("pm_recorder_cap");
    {
        Recorder r(dir.path, 0.0);
        r.write(1, "{}");
    }
    EXPECT_TRUE(std::filesystem::is_empty(dir.path / "c0"));
}

TEST(RecordFile, CompressingDropsATornLastLineAndKeepsEarlierFiles) {
    TempDir dir("pm_record_file");
    const auto raw = dir.path / "20261009T21.jsonl";
    {
        std::ofstream o(raw, std::ios::binary);
        o << "1 {\"a\":1}\n2 {\"b\":2}\n3 {\"c\":";
        o << std::string(100, '\0');  // a crash can leave the tail of a page zero-filled
    }
    std::ofstream(dir.path / "20261009T21.jsonl.zst") << "earlier";
    ASSERT_TRUE(compress_record_file(raw));
    EXPECT_FALSE(std::filesystem::exists(raw));
    ASSERT_TRUE(std::filesystem::exists(dir.path / "20261009T21_1.jsonl.zst"));
    std::string text;
    stream_record_file(dir.path / "20261009T21_1.jsonl.zst", [&](std::string_view d) { text += d; });
    EXPECT_EQ(text, "1 {\"a\":1}\n2 {\"b\":2}\n");

    const auto torn_only = dir.path / "20261009T22.jsonl";
    std::ofstream(torn_only) << "4 {\"d\"";
    ASSERT_TRUE(compress_record_file(torn_only));  // nothing complete: removed, no output
    EXPECT_FALSE(std::filesystem::exists(torn_only));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "20261009T22.jsonl.zst"));
}

TEST(Recorder, StopsInsteadOfBlockingWhenItsRingIsFull) {
    TempDir dir("pm_recorder_full");
    const std::string text(1000, 'x');
    std::size_t n = 0;
    {
        Recorder r(dir.path, 1.0, 1 << 14);
        const auto t0 = std::chrono::steady_clock::now();
        while (!r.overflowed() && n < 1000000) r.write(1, text), ++n;
        EXPECT_TRUE(r.overflowed());
        EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    }
    std::size_t lines = 0;
    stream_record_file(std::filesystem::directory_iterator(dir.path / "c0")->path(), [&](std::string_view d) {
        lines += static_cast<std::size_t>(std::count(d.begin(), d.end(), '\n'));
    });
    EXPECT_LT(lines, n);  // a prefix: everything written before the ring filled
    EXPECT_GT(lines, 0u);
}
