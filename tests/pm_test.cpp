#include "pm/book.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "ref_pm_book.hpp"

using namespace hft::pm;

TEST(PmBook, ParsesPrices) {
    EXPECT_EQ(parse_price("0.366"), 3660);
    EXPECT_EQ(parse_price("0.5"), 5000);
    EXPECT_EQ(parse_price("1"), 10000);
    EXPECT_EQ(parse_price("0.0001"), 1);
    EXPECT_EQ(parse_price("0.123456"), 1234);  // finer than the grid is cut, not rounded
    for (const char* bad : {"", ".", "x", "0.1x", "2.0", "-0.1"}) EXPECT_EQ(parse_price(bad), -1) << bad;
}

TEST(PmBook, AppliesLevelsAndRemovesAtZero) {
    TokenBook b;
    EXPECT_FALSE(b.two_sided());
    b.set(true, 3600, 100);
    b.set(true, 3590, 50);
    b.set(false, 3700, 80);
    ASSERT_TRUE(b.two_sided());
    EXPECT_EQ(b.best_bid(), 3600);
    EXPECT_EQ(b.best_ask(), 3700);
    EXPECT_FALSE(b.crossed());
    b.set(true, 3600, 0);
    EXPECT_EQ(b.best_bid(), 3590);
    b.set(true, 3700, 10);  // a bid at the ask crosses the book
    EXPECT_TRUE(b.crossed());
    b.clear();
    EXPECT_EQ(b.levels(), 0u);
}

TEST(PmBook, DepthWithinWidthOfBest) {
    TokenBook b;
    b.set(true, 5000, 10);
    b.set(true, 4900, 20);
    b.set(true, 4000, 1000);  // too far from the best
    b.set(false, 5100, 5);
    EXPECT_DOUBLE_EQ(b.depth_bid(500), 30.0);
    EXPECT_DOUBLE_EQ(b.depth_ask(500), 5.0);
}

RC_GTEST_PROP(PmBook, MatchesTheMapBook, ()) {
    TokenBook b;
    test::RefTokenBook r;
    const int n = *rc::gen::inRange(1, 400);
    for (int i = 0; i < n; ++i) {
        const int op = *rc::gen::inRange(0, 50);
        if (op == 0) {
            b.clear(), r.clear();
        } else {
            const bool buy = *rc::gen::arbitrary<bool>();
            const std::int32_t px = *rc::gen::oneOf(rc::gen::inRange(0, 10001), rc::gen::inRange(4900, 5100),
                                                     rc::gen::element(0, 63, 64, 4095, 4096, 9983, 9984, 10000));
            const double sz = *rc::gen::element(0.0, 0.0, 1.0, 12.5, 1103.0, 0.01);
            b.set(buy, px, sz), r.set(buy, px, sz);
        }
        RC_ASSERT(b.two_sided() == r.two_sided());
        RC_ASSERT(b.crossed() == r.crossed());
        RC_ASSERT(b.best_bid() == r.best_bid());
        RC_ASSERT(b.best_ask() == r.best_ask());
        RC_ASSERT(b.levels() == r.levels());
        for (const std::int32_t w : {0, 1, 100, 500, 10000}) {
            RC_ASSERT(b.depth_bid(w) == r.depth_bid(w));
            RC_ASSERT(b.depth_ask(w) == r.depth_ask(w));
        }
        const std::int32_t q = *rc::gen::inRange(0, 10001);
        RC_ASSERT(b.size_at(true, q) == r.size_at(true, q));
        RC_ASSERT(b.size_at(false, q) == r.size_at(false, q));
    }
}

TEST(PmBook, IgnoresPricesOffTheGrid) {
    TokenBook b;
    b.set(true, -1, 5);
    b.set(false, 10001, 5);
    EXPECT_EQ(b.levels(), 0u);
    EXPECT_EQ(b.size_at(true, -1), 0.0);
}

#include "pm/maker.hpp"

namespace {

hft::pm::MakerParams params() {
    hft::pm::MakerParams p;
    p.warmup_s = 0;
    p.requote_s = 0;
    return p;
}

// Bids 0.49 (500 shares) and 0.48, asks 0.51 (500) and 0.52.
void seed(hft::pm::TokenMaker& m) {
    m.book.set(true, 4900, 500);
    m.book.set(true, 4800, 500);
    m.book.set(false, 5100, 500);
    m.book.set(false, 5200, 500);
    m.on_snapshot(1'000'000'000);
}

}  // namespace

TEST(PmMaker, QuotesStayInsideTheSpreadAndNeverCross) {
    hft::pm::TokenMaker m(params());
    seed(m);
    ASSERT_GE(m.bid_px(), 0);
    ASSERT_GE(m.ask_px(), 0);
    EXPECT_LT(m.bid_px(), m.ask_px());
    EXPECT_LE(m.bid_px(), 5100 - 100);
    EXPECT_GE(m.ask_px(), 4900 + 100);
}

TEST(PmMaker, TradeThroughFillsTheWholeOrder) {
    hft::pm::TokenMaker m(params());
    seed(m);
    const std::int32_t b = m.bid_px();
    m.on_trade(2'000'000'000, /*taker_buy=*/false, b - 100, 1);
    EXPECT_EQ(m.buys(), 1u);
    EXPECT_DOUBLE_EQ(m.inventory(), 100.0);
    EXPECT_NEAR(m.pnl(), 100 * (0.5 - b * 1e-4), 1e-9);  // bought below the mid
}

TEST(PmMaker, TradeAtOurPriceFillsOnlyAfterTheQueueAhead) {
    auto p = params();
    p.k = 1e6;  // negligible base spread, so the bid joins the best
    hft::pm::TokenMaker m(p);
    seed(m);
    ASSERT_EQ(m.bid_px(), 4900);  // joins the best bid, behind its 500 shares
    m.on_trade(2'000'000'000, false, 4900, 300);
    EXPECT_EQ(m.buys(), 0u);
    m.on_trade(3'000'000'000, false, 4900, 300);  // 200 left ahead, so 100 reach us
    EXPECT_EQ(m.buys(), 1u);
    EXPECT_DOUBLE_EQ(m.inventory(), 100.0);
}

TEST(PmMaker, SnapshotCancelsQuotesAndWarmupDelaysThem) {
    auto p = params();
    p.warmup_s = 10;
    hft::pm::TokenMaker m(p);
    seed(m);
    EXPECT_LT(m.bid_px(), 0);  // still warming up
    m.on_trade(20'000'000'000, true, 5100, 1);
    EXPECT_GE(m.bid_px(), 0);
    m.on_snapshot(20'000'000'001);
    EXPECT_GE(m.bid_px(), 0);  // cancelled, then placed again by the same update
}

#include <zstd.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <string>

#include "pm/reader.hpp"
#include "temp_dir.hpp"

TEST(PmDecoder, DecodesEachKindAndBatches) {
    hft::pm::Decoder d;
    std::vector<std::string> seen;
    auto rec = [&](const Event& e) {
        switch (e.kind) {
            case Kind::Book:
                seen.push_back("book " + std::string(e.asset) + " " + std::to_string(e.bids.size()) + "/" +
                               std::to_string(e.asks.size()) + " tick " + std::to_string(e.tick) + " bid " +
                               std::to_string(e.bids[0].px) + "x" + std::to_string(e.bids[0].size));
                break;
            case Kind::PriceChange:
                for (const auto& c : e.changes)
                    seen.push_back("chg " + std::string(c.asset) + (c.buy ? " B " : " S ") + std::to_string(c.px) + "x" +
                                   std::to_string(c.size) + " " + std::string(c.best_bid));
                break;
            case Kind::Trade:
                seen.push_back("trade " + std::string(e.price) + (e.buy ? " B " : " S ") + std::to_string(e.size));
                break;
            case Kind::Other: seen.push_back("other " + std::string(e.type)); break;
        }
    };
    ASSERT_TRUE(d.decode(R"({"event_type":"book","asset_id":"7","bids":[{"price":"0.36","size":"12.5"}],)"
                         R"("asks":[{"price":"0.4","size":"3"},{"price":"0.41","size":"1"}],"tick_size":"0.01"})",
                         rec));
    ASSERT_TRUE(d.decode(R"([{"event_type":"price_change","price_changes":[{"asset_id":"7","price":"0.37",)"
                         R"("size":"5","side":"BUY","best_bid":"0.37"},{"asset_id":"8","price":"0.6","size":"0","side":"SELL"}]},)"
                         R"({"event_type":"last_trade_price","asset_id":"7","price":"0.4","size":"2","side":"BUY"},)"
                         R"({"event_type":"best_bid_ask"}])",
                         rec));
    EXPECT_EQ(d.fallbacks(), 0u);  // both took the single pass
    EXPECT_FALSE(d.decode("{\"event_type\":", rec));
    EXPECT_EQ(d.fallbacks(), 1u);
    const std::vector<std::string> want = {"book 7 1/2 tick 100 bid 3600x12.500000",
                                           "chg 7 B 3700x5.000000 0.37",
                                           "chg 8 S 6000x0.000000 ",
                                           "trade 0.4 B 2.000000",
                                           "other best_bid_ask"};
    EXPECT_EQ(seen, want);
}

namespace {

// Every field of every event, so two decoders can be compared exactly.
std::string dump(const Event& e) {
    std::string s = std::to_string(static_cast<int>(e.kind)) + "|" + std::string(e.type) + "|" + std::string(e.asset) + "|" +
                    std::to_string(e.tick) + "|" + std::to_string(e.has_changes) + "|" + std::to_string(e.px) + "|" +
                    std::string(e.price) + "|" + std::to_string(e.size) + "|" + std::to_string(e.buy) + "|" +
                    std::to_string(e.exch_ms) + "|" + std::to_string(e.conn);
    for (const auto& l : e.bids) s += "|b" + std::to_string(l.px) + "x" + std::to_string(l.size);
    for (const auto& l : e.asks) s += "|a" + std::to_string(l.px) + "x" + std::to_string(l.size);
    for (const auto& c : e.changes)
        s += "|c" + std::string(c.asset) + "," + std::to_string(c.px) + "x" + std::to_string(c.size) + "," +
             std::to_string(c.buy) + "," + std::string(c.best_bid) + "," + std::string(c.best_ask);
    return s;
}

rc::Gen<std::string> sp() { return rc::gen::element<std::string>("", "", " ", "\n ", "\t"); }

// Mostly what the exchange sends, so the single-pass path runs; rarely an escape or a number
// form that it leaves to the tape.
rc::Gen<std::string> scalar() {
    return rc::gen::weightedOneOf<std::string>(
        {{30, rc::gen::element<std::string>(R"("0.5")", R"("0.001")", R"("12.34")", R"("7")", R"("8")", R"("BUY")",
                                            R"("SELL")", R"("book")", R"("price_change")", R"("last_trade_price")",
                                            R"("_heartbeat")", R"("_reconnect")", R"("1791561630336")", R"("")", "-1",
                                            "3", "0", "12", "true", "null")},
         {1, rc::gen::element<std::string>(R"("a\nb")", R"("x\"y")", "1.5", "1e3", "-0", "01", "+1", "1.", "false",
                                           "1234567890123")}});
}

rc::Gen<std::string> key() {
    return rc::gen::weightedOneOf<std::string>(
        {{30, rc::gen::element<std::string>("event_type", "event_type", "asset_id", "price", "size", "side", "best_bid",
                                            "best_ask", "tick_size", "bids", "asks", "price_changes", "timestamp",
                                            "conn", "market", "hash")},
         {1, rc::gen::element<std::string>("x", R"(price)")}});
}

rc::Gen<std::string> value(int depth);

rc::Gen<std::string> object(int depth) {
    return rc::gen::map(rc::gen::mapcat(rc::gen::inRange<std::size_t>(0, 7),
                                        [depth](std::size_t n) {
                                            return rc::gen::container<std::vector<std::pair<std::string, std::string>>>(
                                                n, rc::gen::pair(key(), value(depth + 1)));
                                        }),
                        [](const std::vector<std::pair<std::string, std::string>>& kv) {
                            std::string s = "{";
                            for (std::size_t i = 0; i < kv.size(); ++i)
                                s += (i ? ", " : "") + std::string("\"") + kv[i].first + "\":" + kv[i].second;
                            return s + "}";
                        });
}

rc::Gen<std::string> value(int depth) {
    if (depth > 3) return scalar();
    return rc::gen::weightedOneOf<std::string>(
        {{6, scalar()},
         {2, rc::gen::map(rc::gen::mapcat(rc::gen::inRange<std::size_t>(0, 4),
                                          [depth](std::size_t n) {
                                              return rc::gen::container<std::vector<std::string>>(n, object(depth));
                                          }),
                          [](const std::vector<std::string>& v) {
                              std::string s = "[";
                              for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
                              return s + "]";
                          })},
         {1, object(depth)}});
}

}  // namespace

RC_GTEST_PROP(PmDecoder, SizesParseExactlyAsFromChars, ()) {
    const auto s = *rc::gen::container<std::string>(rc::gen::element('0', '1', '5', '9', '9', '.', '+', 'e', '-'));
    double want = 0;
    const char* b = s.data() + (!s.empty() && s[0] == '+');
    if (const auto r = std::from_chars(b, s.data() + s.size(), want); r.ptr == b) want = 0;
    const double got = parse_size(s);
    RC_ASSERT(std::memcmp(&got, &want, sizeof got) == 0);
}

// A top-level object that usually names a real event type, at any position.
std::string event_text() {
    std::string o = *object(0);
    if (*rc::gen::inRange(0, 5) == 0) return o;
    const std::string t = *rc::gen::element<std::string>("book", "price_change", "last_trade_price", "_heartbeat");
    const std::string m = "\"event_type\":\"" + t + "\"";
    if (o == "{}") return "{" + m + "}";
    return *rc::gen::arbitrary<bool>() ? "{" + m + "," + o.substr(1) : o.substr(0, o.size() - 1) + "," + m + "}";
}

RC_GTEST_PROP(PmDecoder, SinglePassMatchesTheTape, ()) {
    std::vector<std::string> objs(*rc::gen::inRange<std::size_t>(1, 4));
    for (auto& o : objs) o = event_text();
    std::string text = *sp();
    if (*rc::gen::arbitrary<bool>()) {
        text += "[";
        for (std::size_t i = 0; i < objs.size(); ++i) text += (i ? "," : "") + *sp() + objs[i];
        text += "]";
    } else {
        text += objs[0];
    }
    text += *sp();
    if (*rc::gen::inRange(0, 4) == 0) {  // one corrupted byte
        const auto at = *rc::gen::inRange<std::size_t>(0, text.size());
        text[at] = *rc::gen::element('"', '{', '}', '[', ']', ',', ':', 'x', '\\', ' ');
    }
    Decoder fast, tape;
    std::vector<std::string> a, b;
    const bool ra = fast.decode(text, [&](const Event& e) { a.push_back(dump(e)); });
    const bool rb = tape.decode_tape(text, [&](const Event& e) { b.push_back(dump(e)); });
    RC_TAG(fast.fallbacks() == 0 ? "single pass" : "tape");
    RC_ASSERT(ra == rb);
    RC_ASSERT(a == b);
}

TEST(PmReader, ReadsConnectionsAcrossBlockBoundaries) {
    TempDir dir("pm_reader");
    // Enough lines that every file spans several 1 MiB blocks.
    auto lines = [](int n, std::int64_t t0) {
        std::string s;
        for (int i = 0; i < n; ++i)
            s += std::to_string(t0 + i) + R"( {"event_type":"last_trade_price","asset_id":")" + std::string(70, '1') +
                 R"(","price":"0.5","size":")" + std::to_string(i % 7 + 1) + "\",\"side\":\"BUY\"}\n";
        return s;
    };
    auto zst = [](const std::string& s) {
        std::string out(ZSTD_compressBound(s.size()), '\0');
        out.resize(ZSTD_compress(out.data(), out.size(), s.data(), s.size(), 1));
        return out;
    };
    std::filesystem::create_directories(dir.path / "c0");
    std::filesystem::create_directories(dir.path / "c1");
    const std::string a = lines(20000, 1'000), b = lines(15000, 50'000), c = lines(9000, 2'000'000'000);
    std::ofstream(dir.path / "c0" / "h1.jsonl.zst", std::ios::binary) << zst(a);
    std::ofstream(dir.path / "c0" / "h2.jsonl", std::ios::binary) << b << "garbage-line\n" << "7 {\"x\":";  // no final newline
    std::ofstream(dir.path / "c1" / "h1.jsonl.zst", std::ios::binary) << zst(c);

    std::uint64_t trades = 0;
    double shares = 0;
    const auto st = hft::pm::read_records(dir.path, [&](std::int64_t, const Event& e) {
        if (e.kind == Kind::Trade) ++trades, shares += e.size;
    });
    EXPECT_EQ(st.files, 3u);
    EXPECT_EQ(trades, 44000u);
    EXPECT_EQ(st.messages, 44001u);    // the cut-off last line counts, and fails to parse
    EXPECT_EQ(st.parse_errors, 2u);    // that line and the one without a timestamp
    double want = 0;
    for (const int n : {20000, 15000, 9000})
        for (int i = 0; i < n; ++i) want += i % 7 + 1;
    EXPECT_EQ(shares, want);
    EXPECT_NEAR(st.max_gap_s, 29'001 * 1e-9, 1e-12);  // h1 ends at 20,999 ns, h2 starts at 50,000; c1 is apart
}
