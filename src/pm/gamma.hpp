#pragma once

// Market and event discovery through the exchange's public metadata API (REST, JSON).

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "net/json.hpp"
#include "net/tls.hpp"
#include "pm/rules.hpp"

namespace hft::pm {

inline constexpr const char* kGammaHost = "gamma-api.polymarket.com";
inline constexpr const char* kWsHost = "ws-subscriptions-clob.polymarket.com";
inline constexpr const char* kWsPath = "/ws/market";

struct Market {
    std::string tag, slug, condition, end, title;
    std::vector<std::pair<std::string, std::string>> tokens;  // token id, outcome label
    bool open = true;                                          // not closed, has a book, takes orders
    MarketRules rules;
};

struct EventInfo {
    std::string id, slug;
    bool neg_risk = false;
    std::vector<Market> markets;
    // Exactly one market resolves Yes and every open market can be traded, so the open
    // markets' Yes tokens cover every outcome that can still win.
    bool complete = false;
};

inline std::string utc_time(std::time_t t, const char* fmt) {
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

inline std::string url_encode(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

// A JSON array that the API sends as a string, e.g. "[\"Yes\", \"No\"]".
inline std::vector<std::string> string_array(const net::Json& m, const char* key) {
    std::vector<std::string> out;
    const std::string s = m.str(key);
    if (s.empty()) return out;
    const net::Json a = net::parse_json(s);
    for (const net::Json& e : a.a)
        if (e.type == net::Json::Type::Str) out.push_back(e.s);
    return out;
}

inline Market market_from(const net::Json& m, const std::string& tag) {
    Market mk{tag, m.str("slug"), m.str("conditionId"), m.str("endDate"), m.str("groupItemTitle"), {}, true, {}};
    const auto ids = string_array(m, "clobTokenIds");
    const auto names = string_array(m, "outcomes");
    for (std::size_t i = 0; i < ids.size(); ++i) mk.tokens.emplace_back(ids[i], i < names.size() ? names[i] : std::to_string(i));
    mk.open = !m.flag("closed") && m.flag("active") && m.flag("enableOrderBook") && m.flag("acceptingOrders") &&
              !mk.tokens.empty();
    MarketRules& r = mk.rules;
    if (const double t = m.num("orderPriceMinTickSize"); t > 0 && t <= 0.1) r.tick = static_cast<std::int32_t>(std::lround(t * 1e4));
    if (const double q = m.num("orderMinSize"); q > 0 && q < 1e6) r.min_qty = std::llround(q * 1e6);
    r.neg_risk = m.flag("negRisk");
    if (m.find("feesEnabled")) r.fees = m.flag("feesEnabled");
    if (const net::Json* f = m.find("feeSchedule"); f && f->type == net::Json::Type::Obj) {
        if (const double rate = f->num("rate"); rate >= 0 && rate < 1) r.fee_rate_ppm = static_cast<std::uint32_t>(std::lround(rate * 1e6));
        if (const double e = f->num("exponent"); e >= 0 && e <= 2) r.fee_exp = static_cast<std::uint8_t>(e);
    }
    if (const double d = m.num("secondsDelay"); d > 0 && d < 60) r.delay_ms = static_cast<std::uint16_t>(std::lround(d * 1000));
    return mk;
}

// Top `per_tag` order-book markets by 24 h volume for each tag, ending at least `min_days` out.
inline std::vector<Market> discover_markets(const std::vector<std::string>& tags, int per_tag, int min_days) {
    std::vector<Market> out;
    std::set<std::string> seen;
    const std::string min_end = utc_time(std::time(nullptr) + min_days * 86400L, "%Y-%m-%dT%H:%M:%SZ");
    for (const std::string& tag : tags) {
        const net::Json t = net::parse_json(net::http_get(kGammaHost, "/tags/slug/" + url_encode(tag)));
        std::string id = t.str("id");
        if (id.empty()) id = std::to_string(static_cast<long long>(t.num("id")));
        const net::Json ms = net::parse_json(net::http_get(
            kGammaHost, "/markets?active=true&closed=false&order=volume24hr&ascending=false&limit=80&tag_id=" + id +
                            "&end_date_min=" + url_encode(min_end)));
        int taken = 0;
        for (const net::Json& m : ms.a) {
            if (taken >= per_tag) break;
            if (!m.flag("enableOrderBook")) continue;
            Market mk = market_from(m, tag);
            if (mk.tokens.empty() || !seen.insert(mk.condition).second) continue;
            out.push_back(std::move(mk));
            ++taken;
        }
    }
    return out;
}

inline EventInfo event_from(const net::Json& e) {
    EventInfo ev;
    ev.id = e.str("id");
    if (ev.id.empty()) ev.id = std::to_string(static_cast<long long>(e.num("id")));
    ev.slug = e.str("slug");
    ev.neg_risk = e.flag("negRisk");
    std::size_t open = 0, placeholders = 0;
    if (const net::Json* ms = e.find("markets"))
        for (const net::Json& m : ms->a) {
            Market mk = market_from(m, ev.slug);
            if (mk.open) ++open;
            else if (!m.flag("closed")) ++placeholders;  // listed but not yet tradeable
            ev.markets.push_back(std::move(mk));
        }
    ev.complete = ev.neg_risk && open >= 2 && placeholders == 0;
    return ev;
}

// The `limit` events with the most 24 h volume, as listed (complete or not).
inline std::vector<EventInfo> discover_events(int limit) {
    const net::Json es = net::parse_json(net::http_get(
        kGammaHost, "/events?active=true&closed=false&order=volume24hr&ascending=false&limit=" + std::to_string(limit)));
    std::vector<EventInfo> out;
    for (const net::Json& e : es.a) out.push_back(event_from(e));
    return out;
}

}  // namespace hft::pm
