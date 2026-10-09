#include "net/tape.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdio>
#include <string>

#include "net/json.hpp"

using namespace hft::net;

namespace {

std::string num_text(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%.17g", d);
    return b;
}

// Canonical form of both trees: strings decoded, numbers printed exactly.
std::string dump(const Json& j) {
    switch (j.type) {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return j.b ? "true" : "false";
        case Json::Type::Num: return num_text(j.n);
        case Json::Type::Str: return "<" + j.s + ">";
        case Json::Type::Arr: {
            std::string s = "[";
            for (const Json& e : j.a) s += dump(e) + ",";
            return s + "]";
        }
        case Json::Type::Obj: {
            std::string s = "{";
            for (std::size_t i = 0; i < j.o.size(); ++i) s += "<" + j.keys[i] + ">:" + dump(j.o[i]) + ",";
            return s + "}";
        }
    }
    return {};
}

std::string dump(const JsonTape& t, std::uint32_t i) {
    using T = JsonTape::Type;
    switch (t.type(i)) {
        case T::Null: return "null";
        case T::Bool: return t.boolean(i) ? "true" : "false";
        case T::Num: return num_text(t.num(i));
        case T::Str: return "<" + t.decode(i) + ">";
        case T::Arr: {
            std::string s = "[";
            for (auto c = t.first(i); c != t.end(i); c = t.next(c)) s += dump(t, c) + ",";
            return s + "]";
        }
        case T::Obj: {
            std::string s = "{";
            for (auto c = t.first(i); c != t.end(i); c = t.next(t.next(c)))
                s += "<" + t.decode(c) + ">:" + dump(t, c + 1) + ",";
            return s + "}";
        }
    }
    return {};
}

bool dom_ok(const std::string& s, std::string& out) {
    try {
        out = dump(parse_json(s));
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

rc::Gen<std::string> ws() { return rc::gen::element<std::string>("", "", " ", "\n", " \t ", "\r\n"); }

rc::Gen<std::string> str_gen() {
    const auto piece = rc::gen::oneOf(
        rc::gen::element<std::string>("a", "Z", "0", " ", "{", "}", "[", "]", ":", ",", "\\\"", "\\\\", "\\n", "\\/",
                                      "\\u00e9", "\\ud83d\\ude00", "\\\\\\\"", "\\\\\\\\"),
        rc::gen::map(rc::gen::inRange(1, 70), [](int n) { return std::string(static_cast<std::size_t>(n), 'x'); }));
    return rc::gen::map(rc::gen::container<std::vector<std::string>>(piece), [](const std::vector<std::string>& v) {
        std::string s = "\"";
        for (const auto& p : v) s += p;
        return s + "\"";
    });
}

std::string value_text(int depth) {
    const int kind = *rc::gen::inRange(0, depth > 3 ? 4 : 6);
    switch (kind) {
        case 0: return *rc::gen::element<std::string>("true", "false", "null");
        case 1: return *rc::gen::element<std::string>("0", "-1", "3.25", "1e3", "-0.5E-2", "12345678901234567890", "0.366");
        case 2:
        case 3: return *str_gen();
        case 4: {
            const int n = *rc::gen::inRange(0, 5);
            std::string s = "[" + *ws();
            for (int i = 0; i < n; ++i) s += (i ? "," : "") + *ws() + value_text(depth + 1) + *ws();
            return s + "]";
        }
        default: {
            const int n = *rc::gen::inRange(0, 5);
            std::string s = "{" + *ws();
            for (int i = 0; i < n; ++i) s += (i ? "," : "") + *ws() + *str_gen() + *ws() + ":" + *ws() + value_text(depth + 1);
            return s + *ws() + "}";
        }
    }
}

void expect_same(const std::string& s) {
    std::string want;
    const bool ok = dom_ok(s, want);
    for (const bool simd : {false, true}) {
        JsonTape t;
        t.use_simd(simd);
        const bool got = t.parse(s);
        RC_ASSERT(got == ok);
        if (ok) RC_ASSERT(dump(t, t.root()) == want);
    }
}

}  // namespace

TEST(Tape, ReadsAnExchangeMessage) {
    const std::string m =
        R"([{"event_type":"price_change","price_changes":[{"asset_id":"5123","price":"0.371","size":"0","side":"BUY"},)"
        R"({"asset_id":"8732","price":"0.629","size":"1103.5","side":"SELL"}],"timestamp":"1791543601154"}])";
    JsonTape t;
    ASSERT_TRUE(t.parse(m));
    ASSERT_EQ(t.type(0), JsonTape::Type::Arr);
    const auto e = t.first(0);
    EXPECT_EQ(t.str(e, "event_type"), "price_change");
    const auto pc = t.find(e, "price_changes");
    ASSERT_NE(pc, JsonTape::npos);
    EXPECT_EQ(t.size(pc), 2u);
    const auto second = t.next(t.first(pc));
    EXPECT_EQ(t.str(second, "size"), "1103.5");
    EXPECT_EQ(t.str(second, "side"), "SELL");
    EXPECT_EQ(t.find(e, "missing"), JsonTape::npos);
    EXPECT_EQ(t.next(e), t.end(0));
}

TEST(Tape, EscapesAcrossBlockBoundaries) {
    for (int pad = 0; pad < 70; ++pad) {
        const std::string s = "{\"" + std::string(static_cast<std::size_t>(pad), 'x') + "\\\\\\\"k\":[1,\"\\\\\"]}";
        std::string want;
        ASSERT_TRUE(dom_ok(s, want));
        for (const bool simd : {false, true}) {
            JsonTape t;
            t.use_simd(simd);
            ASSERT_TRUE(t.parse(s)) << pad;
            EXPECT_EQ(dump(t, 0), want) << pad;
        }
    }
}

TEST(Tape, RejectsMalformedInput) {
    for (const char* s : {"", "{", "}", "[1,]", "{\"a\"}", "{\"a\":}", "{\"a\" 1}", "[1 2]", "\"abc", "tru", "nul",
                          "[1]x", "{\"a\":1,}", "[\"a\\\"]", "1.2.3", "{a:1}", "[--1]"}) {
        JsonTape t;
        EXPECT_FALSE(t.parse(s)) << s;
    }
}

RC_GTEST_PROP(Tape, MatchesTheTreeReader, ()) { expect_same(*rc::gen::exec([] { return value_text(0); })); }

RC_GTEST_PROP(Tape, AgreesOnAcceptanceAfterOneEdit, ()) {
    std::string s = *rc::gen::exec([] { return value_text(0); });
    const auto at = *rc::gen::inRange<std::size_t>(0, s.size() + 1);
    const char c = *rc::gen::element('"', '\\', '{', '}', '[', ']', ':', ',', ' ', 'a', '1', '\0');
    if (*rc::gen::arbitrary<bool>() && at < s.size()) s.erase(at, 1);
    else s.insert(at, 1, c);
    expect_same(s);
}
