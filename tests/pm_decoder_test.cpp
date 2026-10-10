#include "pm/msg.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <zstd.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <string>

#include "pm/reader.hpp"
#include "temp_dir.hpp"

using namespace hft::pm;

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
