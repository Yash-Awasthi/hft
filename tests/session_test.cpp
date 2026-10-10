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
    s.tokens = {{"111", "Team A\tYes", 0}, {"222", "Team A No", 0}, {"333", "Team B Yes", 1}};
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
