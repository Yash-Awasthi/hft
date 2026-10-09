#pragma once

// Reference for pm::TokenBook: the same book on two std::maps.

#include <cstdint>
#include <functional>
#include <map>

namespace hft::pm::test {

class RefTokenBook {
   public:
    void clear() { bids_.clear(), asks_.clear(); }

    // A size of zero removes the level.
    void set(bool buy, std::int32_t px, double size) {
        if (buy) put(bids_, px, size);
        else put(asks_, px, size);
    }

    // Shares displayed at `px` on one side, zero if the level is empty.
    double size_at(bool buy, std::int32_t px) const {
        if (buy) {
            const auto it = bids_.find(px);
            return it == bids_.end() ? 0.0 : it->second;
        }
        const auto it = asks_.find(px);
        return it == asks_.end() ? 0.0 : it->second;
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

}  // namespace hft::pm::test
