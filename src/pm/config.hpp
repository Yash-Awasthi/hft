#pragma once

// Run configuration (exec.cfg): "key = value" lines, '#' starts a comment. Parsing is strict
// (SECURITY SEC4): an unknown or repeated key, a value that is not entirely a finite number, a
// fraction for an integer key, or a value outside the key's hard bounds refuses the run.

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pm/engine.hpp"

namespace hft::pm {

namespace config_detail {

struct Key {
    const char* name;
    double lo, hi;
    bool integer;
    void (*set)(EngineParams&, double);
};

inline exec::Usd usd(double v) { return std::llround(v * static_cast<double>(exec::kDollar)); }
inline exec::Ns ms(double v) { return std::llround(v * 1e6); }
inline exec::Ns sec(double v) { return std::llround(v * 1e9); }
inline std::uint32_t ppm(double v) { return static_cast<std::uint32_t>(std::lround(v * 1e6)); }

// Parity with the makers' own fill model (D25): side from the feed, shorting allowed, and no
// limit the old model did not have.
inline void compat(EngineParams& p, double on) {
    if (on == 0) return;
    p.sim.compat_side = true, p.long_only = false;
    exec::RiskLimits& r = p.risk;
    r.allow_short = true, r.collar_ticks = 10'000, r.max_open_per_token = r.max_open = 1u << 30;
    r.token_cap = r.group_cap = r.gross_cap = r.daily_stop = 100'000'000 * exec::kDollar;
    r.order_burst = r.sustained_burst = r.cancel_burst = 1'000'000'000;
    r.order_rate_per_s = r.sustained_rate_per_s = r.cancel_rate_per_s = 1'000'000'000;
    r.reject_window = 0;  // a zero window holds no rejects: R13 off
}

inline constexpr double kMaxUsd = 10'000'000;  // ten times the D3 desk; far inside D5 headroom

inline const Key kKeys[] = {
    // venue model
    {"seed", 0, 9.0e15, true, [](EngineParams& p, double v) { p.sim.seed = static_cast<std::uint64_t>(v); }},
    {"lat_ms", 0, 10'000, false, [](EngineParams& p, double v) { p.sim.lat_in = p.sim.lat_out = ms(v); }},
    {"jitter_ms", 0, 10'000, false, [](EngineParams& p, double v) { p.sim.jitter = ms(v); }},
    {"settle_ms", 0, 600'000, false, [](EngineParams& p, double v) { p.sim.settle_delay = ms(v); }},
    {"p_settle_fail", 0, 1, false, [](EngineParams& p, double v) { p.sim.p_settle_fail_ppm = ppm(v); }},
    {"p_drop_ack", 0, 1, false, [](EngineParams& p, double v) { p.sim.p_drop_ack_ppm = ppm(v); }},
    {"p_drop_fill", 0, 1, false, [](EngineParams& p, double v) { p.sim.p_drop_fill_ppm = ppm(v); }},
    {"p_dup", 0, 1, false, [](EngineParams& p, double v) { p.sim.p_dup_ppm = ppm(v); }},
    {"disconnect_every_s", 0, 86'400, false, [](EngineParams& p, double v) { p.sim.disc_every = sec(v); }},
    {"disconnect_for_s", 0, 3'600, false, [](EngineParams& p, double v) { p.sim.disc_for = sec(v); }},
    {"compat", 0, 1, true, compat},
    // account and risk (D3)
    {"capital_usd", 1, kMaxUsd, false, [](EngineParams& p, double v) { p.capital = usd(v); }},
    {"token_cap_usd", 0, kMaxUsd, false, [](EngineParams& p, double v) { p.risk.token_cap = usd(v); }},
    {"group_cap_usd", 0, kMaxUsd, false, [](EngineParams& p, double v) { p.risk.group_cap = usd(v); }},
    {"gross_cap_usd", 0, kMaxUsd, false, [](EngineParams& p, double v) { p.risk.gross_cap = usd(v); }},
    {"daily_stop_usd", 0, kMaxUsd, false, [](EngineParams& p, double v) { p.risk.daily_stop = usd(v); }},
    {"max_order_shares", 0, 1'000'000, false, [](EngineParams& p, double v) { p.risk.max_order_qty = std::llround(v * 1e6); }},
    {"collar_ticks", 0, 10'000, true, [](EngineParams& p, double v) { p.risk.collar_ticks = static_cast<exec::Px>(v); }},
    {"max_open_per_token", 1, 10'000, true, [](EngineParams& p, double v) { p.risk.max_open_per_token = static_cast<std::uint32_t>(v); }},
    {"max_open", 1, 1'000'000, true, [](EngineParams& p, double v) { p.risk.max_open = static_cast<std::uint32_t>(v); }},
    {"reject_spike", 1, 1'000'000, true, [](EngineParams& p, double v) { p.risk.reject_spike = static_cast<std::uint32_t>(v); }},
    {"reject_window_s", 0.001, 3'600, false, [](EngineParams& p, double v) { p.risk.reject_window = sec(v); }},
    // maker
    {"quote", 0, 1, true, [](EngineParams& p, double v) { p.quote = v != 0; }},
    {"gamma", 1e-6, 100, false, [](EngineParams& p, double v) { p.maker.gamma = v; }},
    {"k", 1e-6, 1e6, false, [](EngineParams& p, double v) { p.maker.k = v; }},
    {"size", 1, 100'000, false, [](EngineParams& p, double v) { p.maker.size = v; }},
    {"max_inv", 0, 10'000'000, false, [](EngineParams& p, double v) { p.maker.max_inv = v; }},
    {"horizon_s", 1, 1e6, false, [](EngineParams& p, double v) { p.maker.horizon_s = v; }},
    {"min_mid", 0, 1, false, [](EngineParams& p, double v) { p.maker.min_mid = v; }},
    {"max_mid", 0, 1, false, [](EngineParams& p, double v) { p.maker.max_mid = v; }},
    {"warmup_s", 0, 86'400, false, [](EngineParams& p, double v) { p.maker.warmup_s = v; }},
    {"requote_s", 0, 3'600, false, [](EngineParams& p, double v) { p.maker.requote_s = v; }},
    // engine
    {"loss_stop_usd", 0, kMaxUsd, false, [](EngineParams& p, double v) { p.loss_stop_usd = v; }},
    {"max_gross_shares", 0, 1e8, false, [](EngineParams& p, double v) { p.max_gross_shares = v; }},
    {"stale_s", 0.1, 3'600, false, [](EngineParams& p, double v) { p.stale_s = v; }},
};

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace config_detail

// Applies `text` to `p`; errors name `origin` and the line. Separate calls may set a key again
// (command-line overrides after the file), one call may not.
inline void apply_config(EngineParams& p, std::string_view text, const std::string& origin) {
    using namespace config_detail;
    std::vector<const Key*> seen;
    int n = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++n;
        auto fail = [&](const std::string& why) { throw std::runtime_error(origin + ":" + std::to_string(n) + ": " + why); };
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) fail("expected key = value");
        const std::string_view name = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        const Key* key = nullptr;
        for (const Key& k : kKeys)
            if (name == k.name) key = &k;
        if (!key) fail("unknown key " + std::string(name));
        for (const Key* s : seen)
            if (s == key) fail("repeated key " + std::string(name));
        seen.push_back(key);
        double v = 0;
        const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
        if (val.empty() || ec != std::errc{} || end != val.data() + val.size() || !std::isfinite(v))
            fail("not a number: " + std::string(name) + " = " + std::string(val));
        if (key->integer && v != std::floor(v)) fail(std::string(name) + " must be an integer");
        if (v < key->lo || v > key->hi)
            fail(std::string(name) + " = " + std::string(val) + " outside [" + std::to_string(key->lo) + ", " + std::to_string(key->hi) + "]");
        key->set(p, v);
    }
}

}  // namespace hft::pm
