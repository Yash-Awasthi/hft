#pragma once

// Level-2 book of one prediction-market outcome token, rebuilt from the exchange's snapshot
// ("book") and delta ("price_change") messages. Prices are integers in units of 0.0001
// (the contract pays 1.0000), sizes are shares.
//
// Every price on the grid has a slot; a two-level bitmap of non-empty slots gives the best
// price with one leading/trailing-zero count per level.

#include <cstdint>
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
    static constexpr std::int32_t kMaxPx = 10000;

    void clear() { bids_.clear(), asks_.clear(); }

    // A size of zero removes the level. Prices off the grid are ignored.
    void set(bool buy, std::int32_t px, double size) {
        if (px < 0 || px > kMaxPx) return;
        (buy ? bids_ : asks_).put(px, size);
    }

    // Shares displayed at `px` on one side, zero if the level is empty.
    double size_at(bool buy, std::int32_t px) const {
        if (px < 0 || px > kMaxPx) return 0.0;
        return (buy ? bids_ : asks_).size[px];
    }

    bool two_sided() const { return bids_.count && asks_.count; }
    bool crossed() const { return two_sided() && best_bid() >= best_ask(); }
    std::int32_t best_bid() const { return bids_.highest(); }
    std::int32_t best_ask() const { return asks_.lowest(); }

    // Shares resting within `width` (price units) of the best on each side, summed from the
    // best outward.
    double depth_bid(std::int32_t width) const {
        const std::int32_t best = best_bid();
        return best < 0 ? 0.0 : bids_.sum_down(best, best - width);
    }
    double depth_ask(std::int32_t width) const {
        const std::int32_t best = best_ask();
        return best < 0 ? 0.0 : asks_.sum_up(best, best + width);
    }

    std::size_t levels() const { return bids_.count + asks_.count; }

    // Walking the book outward: the next non-empty ask at or above px, bid at or below px; -1 if none.
    std::int32_t ask_at_or_above(std::int32_t px) const { return px > kMaxPx ? -1 : asks_.at_or_above(px < 0 ? 0 : px); }
    std::int32_t bid_at_or_below(std::int32_t px) const { return px < 0 ? -1 : bids_.at_or_below(px > kMaxPx ? kMaxPx : px); }

   private:
    struct Side {
        static constexpr int kWords = (kMaxPx + 64) / 64;
        static constexpr int kTop = (kWords + 63) / 64;
        double size[kMaxPx + 1] = {};
        std::uint64_t bits[kWords] = {};
        std::uint64_t top[kTop] = {};
        std::uint32_t count = 0;
        std::int32_t hi = -1, lo = -1;  // highest and lowest non-empty prices, kept on every put

        void put(std::int32_t px, double sz) {
            const int w = px >> 6;
            const std::uint64_t bit = 1ull << (px & 63);
            if (sz > 0) {
                count += !(bits[w] & bit);
                bits[w] |= bit;
                top[w >> 6] |= 1ull << (w & 63);
                size[px] = sz;
                if (px > hi) hi = px;
                if (lo < 0 || px < lo) lo = px;
            } else if (bits[w] & bit) {
                --count;
                bits[w] &= ~bit;
                if (!bits[w]) top[w >> 6] &= ~(1ull << (w & 63));
                size[px] = 0;
                if (px == hi) hi = at_or_below(px);
                if (px == lo) lo = at_or_above(px);
            }
        }
        void clear() {
            for (int t = 0; t < kTop; ++t)
                for (std::uint64_t m = top[t]; m; m &= m - 1) {
                    const int w = t * 64 + __builtin_ctzll(m);
                    for (std::uint64_t b = bits[w]; b; b &= b - 1) size[w * 64 + __builtin_ctzll(b)] = 0;
                    bits[w] = 0;
                }
            for (auto& t : top) t = 0;
            count = 0;
            hi = lo = -1;
        }
        // Highest non-empty price at or below px, -1 if none.
        std::int32_t at_or_below(std::int32_t px) const {
            if (px < 0) return -1;
            int w = px >> 6;
            std::uint64_t m = bits[w] & (~0ull >> (63 - (px & 63)));
            if (m) return w * 64 + 63 - __builtin_clzll(m);
            for (int t = w >> 6; t >= 0; --t) {
                std::uint64_t tm = top[t];
                if (t == w >> 6) tm &= (w & 63) ? ~0ull >> (64 - (w & 63)) : 0;
                if (tm) {
                    w = t * 64 + 63 - __builtin_clzll(tm);
                    return w * 64 + 63 - __builtin_clzll(bits[w]);
                }
            }
            return -1;
        }
        // Lowest non-empty price at or above px, -1 if none.
        std::int32_t at_or_above(std::int32_t px) const {
            if (px > kMaxPx) return -1;
            int w = px >> 6;
            std::uint64_t m = bits[w] & (~0ull << (px & 63));
            if (m) return w * 64 + __builtin_ctzll(m);
            for (int t = w >> 6; t < kTop; ++t) {
                std::uint64_t tm = top[t];
                if (t == w >> 6) tm &= (w & 63) == 63 ? 0 : ~0ull << ((w & 63) + 1);
                if (tm) {
                    w = t * 64 + __builtin_ctzll(tm);
                    return w * 64 + __builtin_ctzll(bits[w]);
                }
            }
            return -1;
        }
        // Sizes from `hi` down to `lo` (inclusive, clamped to the grid), highest price first.
        double sum_down(std::int32_t hi, std::int32_t lo) const {
            if (lo < 0) lo = 0;
            double d = 0;
            for (int w = hi >> 6; w >= (lo >> 6); --w) {
                std::uint64_t m = bits[w];
                if (w == hi >> 6) m &= ~0ull >> (63 - (hi & 63));
                if (w == lo >> 6) m &= ~0ull << (lo & 63);
                for (; m; m &= ~(1ull << (63 - __builtin_clzll(m)))) d += size[w * 64 + 63 - __builtin_clzll(m)];
            }
            return d;
        }
        // Sizes from `lo` up to `hi` (inclusive, clamped to the grid), lowest price first.
        double sum_up(std::int32_t lo, std::int32_t hi) const {
            if (hi > kMaxPx) hi = kMaxPx;
            double d = 0;
            for (int w = lo >> 6; w <= (hi >> 6); ++w) {
                std::uint64_t m = bits[w];
                if (w == lo >> 6) m &= ~0ull << (lo & 63);
                if (w == hi >> 6) m &= ~0ull >> (63 - (hi & 63));
                for (; m; m &= m - 1) d += size[w * 64 + __builtin_ctzll(m)];
            }
            return d;
        }
        std::int32_t highest() const { return hi; }
        std::int32_t lowest() const { return lo; }
    };

    Side bids_, asks_;
};

}  // namespace hft::pm
