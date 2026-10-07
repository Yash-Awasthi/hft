#include <gtest/gtest.h>

#include <string>

#include "experiment/config.hpp"
#include "experiment/registry.hpp"
#include "experiment/splits.hpp"
#include "temp_dir.hpp"

using namespace hft::experiment;

namespace {

const char* kGood = R"(
[run]
name = "smoke"
question = "Q1"
seed = 42

[data]
days = ["2025-12-08", "2025-12-09"]
symbols = ["AAPL", "MSFT"]

[exchange]
tick = 100
maker_rebate = 2000
taker_fee = 3000
stp = "cancel_oldest"
lock = "reprice"
fill_rule = "trade_through"

[latency]
market_data_ns = 50000
order_entry_ns = 20000

[strategy]
name = "naive"
params = { size = 100, edge = 0.5 }
)";

const char* kSplits = R"(
[train]
days = ["2025-12-08", "2025-12-09"]
[validation]
days = ["2025-12-11"]
[test]
locked = true
days = ["2025-12-12"]
[robustness]
days = ["2025-11-28"]
[files]
"2025-12-08" = "S120825-v50.txt.gz"
)";

std::string replace(std::string s, const std::string& from, const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
}

}  // namespace

TEST(Config, ParsesAndHashesCanonically) {
    const RunConfig c = parse_config(kGood);
    EXPECT_EQ(c.name, "smoke");
    EXPECT_EQ(c.seed, 42u);
    EXPECT_EQ(c.days.size(), 2u);
    EXPECT_EQ(c.exchange.stp, hft::engine::Stp::CancelOldest);
    EXPECT_EQ(c.exchange.lock, hft::engine::LockPolicy::Reprice);
    EXPECT_EQ(c.fill_rule, hft::engine::FillRule::TradeThrough);
    EXPECT_EQ(c.market_data_ns, 50000u);
    EXPECT_EQ(c.processing_ns, 0u);  // default
    EXPECT_EQ(c.hash.size(), 64u);
    // Whitespace and key order do not change the hash; a value does.
    const std::string reordered = replace(kGood, "maker_rebate = 2000\ntaker_fee = 3000",
                                          "taker_fee   = 3000\nmaker_rebate = 2000");
    EXPECT_EQ(parse_config(reordered).hash, c.hash);
    EXPECT_NE(parse_config(replace(kGood, "seed = 42", "seed = 43")).hash, c.hash);
    EXPECT_NE(parse_config(replace(kGood, "size = 100", "size = 101")).hash, c.hash);
}

TEST(Config, RejectsSchemaViolations) {
    const std::pair<std::string, std::string> bad[] = {
        {"seed = 42", "seed = \"x\""},                   // wrong type
        {"tick = 100", "tick = 7"},                      // not an allowed tick
        {"stp = \"cancel_oldest\"", "stp = \"maybe\""},  // unknown enum
        {"[latency]", "[latency]\nmystery = 1"},         // unknown key
        {"name = \"smoke\"\n", ""},                      // missing required key
        {"market_data_ns = 50000", "market_data_ns = -5"},
        {"days = [\"2025-12-08\", \"2025-12-09\"]", "days = [\"12/08/2025\"]"},
    };
    for (const auto& [from, to] : bad)
        EXPECT_THROW(parse_config(replace(kGood, from, to)), ConfigError) << to;
}

TEST(Splits, LocksTestDays) {
    const Splits s = parse_splits(kSplits);
    EXPECT_EQ(s.of("2025-12-08"), Split::Train);
    EXPECT_EQ(s.of("2025-12-12"), Split::Test);
    EXPECT_EQ(s.of("2025-11-28"), Split::Robustness);
    EXPECT_THROW(s.of("2024-01-01"), ConfigError);
    RunConfig c = parse_config(kGood);
    EXPECT_FALSE(s.touches_test(c));
    c.days.push_back("2025-12-12");
    EXPECT_TRUE(s.touches_test(c));
    EXPECT_THROW(s.check_access(c), ConfigError);
    c.allow_test = true;
    EXPECT_NO_THROW(s.check_access(c));
}

TEST(Splits, RepositoryFileParses) {
    const Splits s = load_splits(HFT_SOURCE_DIR "/configs/splits.toml");
    EXPECT_EQ(s.of("2025-12-11"), Split::Validation);
    EXPECT_EQ(s.of("2026-06-12"), Split::Test);
}

TEST(Registry, RecordsRunsCountsTrialsAndAuditsTestAccess) {
    TempDir dir("registry");
    const std::string db = (dir.path / "runs.sqlite").string();
    RunConfig c = parse_config(kGood);
    {
        Registry r(db);
        const auto a = r.begin(c, "abc123", "datahash", false);
        r.finish(a, R"({"pnl": 1.5})", "out/a");
        r.begin(c, "abc123", "datahash", false);
        c.question = "Q2";
        c.days.push_back("2025-12-12");
        c.allow_test = true;
        r.begin(c, "abc123", "datahash", true);
    }
    Registry r(db);  // reopens the same file
    EXPECT_EQ(r.count("Q1"), 2);
    EXPECT_EQ(r.count("Q2"), 1);
    const auto audit = r.test_accesses();
    ASSERT_EQ(audit.size(), 1u);
    EXPECT_EQ(audit[0].question, "Q2");
    EXPECT_EQ(audit[0].config_hash, c.hash);
}

TEST(Config, RepositoryExampleIsValidAndOnTrainDays) {
    const RunConfig c = load_config(HFT_SOURCE_DIR "/configs/example.toml");
    const Splits s = load_splits(HFT_SOURCE_DIR "/configs/splits.toml");
    EXPECT_NO_THROW(s.check_access(c));
    EXPECT_FALSE(s.touches_test(c));
    EXPECT_EQ(s.of(c.days[0]), Split::Train);
}
