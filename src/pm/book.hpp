#pragma once

// Level-2 book of one prediction-market outcome token, rebuilt from the exchange's snapshot
// ("book") and delta ("price_change") messages. Prices are integers in units of 0.0001
// (the contract pays 1.0000), sizes are shares.

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string_view>

namespace hft::pm {

// "0.366" -> 3660. Returns -1 on a malformed price.
inline std::int32_t parse_price(std::string_view s) {
    std::int32_t whole = 0, frac = 0, digits = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) whole = whole * 10 + (s[i] - '0');
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
            if (digits < 4) frac = frac * 10 + (s[i] - '0'), ++digits;
    }
    if (i != s.size() || s.empty() || s == "." || whole > 1 || (whole == 1 && frac > 0)) return -1;
    while (digits++ < 4) frac *= 10;
    return whole * 10000 + frac;
}

class TokenBook {
   public:
    void clear() { bids_.clear(), asks_.clear(); }

    // A size of zero removes the level.
    void set(bool buy, std::int32_t px, double size) {
        if (buy) put(bids_, px, size);
        else put(asks_, px, size);
    }

    bool two_sided() const { return !bids_.empty() && !asks_.empty(); }
    bool crossed() const { return two_sided() && bids_.begin()->first >= asks_.begin()->first; }
    std::int32_t best_bid() const { return bids_.empty() ? -1 : bids_.begin()->first; }
    std::int32_t best_ask() const { return asks_.empty() ? -1 : asks_.begin()->first; }

    // Shares resting within `width` (price units) of the best on each side.
    double depth_bid(std::int32_t width) const { return depth(bids_, width); }
    double depth_ask(std::int32_t width) const { return depth(asks_, width); }

    std::size_t levels() const { return bids_.size() + asks_.size(); }

   private:
    template <class M>
    static void put(M& s, std::int32_t px, double size) {
        if (size > 0) s[px] = size;
        else s.erase(px);
    }
    template <class M>
    static double depth(const M& s, std::int32_t width) {
        if (s.empty()) return 0;
        const std::int32_t best = s.begin()->first;
        double d = 0;
        for (const auto& [px, sz] : s) {
            if ((px > best ? px - best : best - px) > width) break;
            d += sz;
        }
        return d;
    }
    std::map<std::int32_t, double, std::greater<std::int32_t>> bids_;
    std::map<std::int32_t, double> asks_;
};

}  // namespace hft::pm
