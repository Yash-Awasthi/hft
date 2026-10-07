#include "experiment/config.hpp"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <toml++/toml.hpp>

#include "core/sha256.hpp"

namespace hft::experiment {

namespace {

[[noreturn]] void fail(const std::string& msg) { throw ConfigError("config: " + msg); }

const toml::table& table(const toml::table& root, const char* key, bool required) {
    static const toml::table empty;
    const toml::node* n = root.get(key);
    if (!n) {
        if (required) fail(std::string("missing table [") + key + "]");
        return empty;
    }
    if (!n->is_table()) fail(std::string(key) + " must be a table");
    return *n->as_table();
}

void only(const toml::table& t, const char* where, std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : t)
        if (std::find(keys.begin(), keys.end(), k.str()) == keys.end())
            fail(std::string("unknown key ") + where + "." + std::string(k.str()));
}

std::string str(const toml::table& t, const char* where, const char* key, bool required,
                std::string def = {}) {
    const toml::node* n = t.get(key);
    if (!n) {
        if (required) fail(std::string("missing ") + where + "." + key);
        return def;
    }
    if (!n->is_string()) fail(std::string(where) + "." + key + " must be a string");
    return n->as_string()->get();
}

std::int64_t integer(const toml::table& t, const char* where, const char* key, std::int64_t def,
                     std::int64_t lo, std::int64_t hi) {
    const toml::node* n = t.get(key);
    if (!n) return def;
    if (!n->is_integer()) fail(std::string(where) + "." + key + " must be an integer");
    const std::int64_t v = n->as_integer()->get();
    if (v < lo || v > hi)
        fail(std::string(where) + "." + key + " out of range [" + std::to_string(lo) + ", " +
             std::to_string(hi) + "]");
    return v;
}

std::vector<std::string> strings(const toml::table& t, const char* where, const char* key,
                                 bool required) {
    const toml::node* n = t.get(key);
    if (!n) {
        if (required) fail(std::string("missing ") + where + "." + key);
        return {};
    }
    const toml::array* a = n->as_array();
    if (!a) fail(std::string(where) + "." + key + " must be an array of strings");
    std::vector<std::string> out;
    for (const auto& e : *a) {
        if (!e.is_string()) fail(std::string(where) + "." + key + " must be an array of strings");
        out.push_back(e.as_string()->get());
    }
    return out;
}

template <class E>
E pick(const std::string& v, const char* what, std::initializer_list<std::pair<const char*, E>> m) {
    for (const auto& [name, e] : m)
        if (v == name) return e;
    fail(std::string("unknown ") + what + " \"" + v + "\"");
}

}  // namespace

RunConfig parse_config(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        fail(std::string("TOML syntax: ") + std::string(e.description()));
    }
    only(root, "", {"run", "data", "exchange", "latency", "strategy"});
    RunConfig c;

    const toml::table& run = table(root, "run", true);
    only(run, "run", {"name", "question", "seed"});
    c.name = str(run, "run", "name", true);
    c.question = str(run, "run", "question", true);
    c.seed = static_cast<std::uint64_t>(integer(run, "run", "seed", 0, 0, INT64_MAX));

    const toml::table& data = table(root, "data", true);
    only(data, "data", {"days", "symbols", "allow_test"});
    c.days = strings(data, "data", "days", true);
    c.symbols = strings(data, "data", "symbols", false);
    static const std::regex day(R"(\d{4}-\d{2}-\d{2})");
    for (const auto& d : c.days)
        if (!std::regex_match(d, day)) fail("data.days: \"" + d + "\" is not YYYY-MM-DD");
    if (c.days.empty()) fail("data.days is empty");
    if (const toml::node* n = data.get("allow_test")) {
        if (!n->is_boolean()) fail("data.allow_test must be a boolean");
        c.allow_test = n->as_boolean()->get();
    }

    const toml::table& ex = table(root, "exchange", false);
    only(ex, "exchange", {"tick", "maker_rebate", "taker_fee", "stp", "lock", "fill_rule"});
    c.exchange.tick = static_cast<std::uint32_t>(integer(ex, "exchange", "tick", 100, 1, 100));
    if (c.exchange.tick != 1 && c.exchange.tick != 50 && c.exchange.tick != 100)
        fail("exchange.tick must be 1, 50 or 100");
    c.exchange.maker_rebate = integer(ex, "exchange", "maker_rebate", 0, -100'000, 100'000);
    c.exchange.taker_fee = integer(ex, "exchange", "taker_fee", 0, -100'000, 100'000);
    using engine::FillRule, engine::LockPolicy, engine::Stp;
    c.exchange.stp = pick<Stp>(str(ex, "exchange", "stp", false, "cancel_newest"), "stp",
                               {{"none", Stp::None},
                                {"cancel_newest", Stp::CancelNewest},
                                {"cancel_oldest", Stp::CancelOldest},
                                {"cancel_both", Stp::CancelBoth}});
    c.exchange.lock = pick<LockPolicy>(str(ex, "exchange", "lock", false, "reject"), "lock",
                                       {{"reject", LockPolicy::Reject},
                                        {"reprice", LockPolicy::Reprice},
                                        {"allow_lock", LockPolicy::AllowLock}});
    c.fill_rule =
        pick<FillRule>(str(ex, "exchange", "fill_rule", false, "queue"), "fill_rule",
                       {{"queue", FillRule::Queue}, {"trade_through", FillRule::TradeThrough}});

    const toml::table& lat = table(root, "latency", false);
    only(lat, "latency", {"market_data_ns", "order_entry_ns", "processing_ns"});
    constexpr std::int64_t kSecond = 1'000'000'000;
    c.market_data_ns =
        static_cast<std::uint64_t>(integer(lat, "latency", "market_data_ns", 0, 0, kSecond));
    c.order_entry_ns =
        static_cast<std::uint64_t>(integer(lat, "latency", "order_entry_ns", 0, 0, kSecond));
    c.processing_ns =
        static_cast<std::uint64_t>(integer(lat, "latency", "processing_ns", 0, 0, kSecond));

    const toml::table& st = table(root, "strategy", true);
    only(st, "strategy", {"name", "params"});
    c.strategy = str(st, "strategy", "name", true);
    if (const toml::node* p = st.get("params")) {
        if (!p->is_table()) fail("strategy.params must be a table");
        std::ostringstream os;
        os << *p->as_table();  // toml++ tables are key-ordered, so this is canonical
        c.strategy_params = os.str();
    }

    std::ostringstream cs;
    auto list = [&](const std::vector<std::string>& v) {
        for (const auto& s : v) cs << s << ',';
        cs << '\n';
    };
    cs << "name=" << c.name << "\nquestion=" << c.question << "\nseed=" << c.seed << "\ndays=";
    list(c.days);
    cs << "symbols=";
    list(c.symbols);
    cs << "allow_test=" << c.allow_test << "\ntick=" << c.exchange.tick
       << "\nmaker_rebate=" << c.exchange.maker_rebate << "\ntaker_fee=" << c.exchange.taker_fee
       << "\nstp=" << static_cast<int>(c.exchange.stp)
       << "\nlock=" << static_cast<int>(c.exchange.lock)
       << "\nfill_rule=" << static_cast<int>(c.fill_rule) << "\nmarket_data_ns=" << c.market_data_ns
       << "\norder_entry_ns=" << c.order_entry_ns << "\nprocessing_ns=" << c.processing_ns
       << "\nstrategy=" << c.strategy << "\nparams=" << c.strategy_params << '\n';
    c.canonical = cs.str();
    Sha256 h;
    h.update(reinterpret_cast<const std::uint8_t*>(c.canonical.data()), c.canonical.size());
    c.hash = h.hex();
    return c;
}

RunConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) fail("cannot open " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return parse_config(s.str());
}

}  // namespace hft::experiment
