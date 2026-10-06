#pragma once

// Main L3 book. Price levels live in a dense window of kLevels ticks per side with a
// two-level bitmap of non-empty levels, so the best level is two lzcnt or tzcnt. Prices off
// the tick grid or outside the window sit in a small sorted overflow array per side. Orders
// are index-addressed: hot fields (price, quantity, links) in 16 bytes, cold fields apart.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "book/id_map.hpp"
#include "book/types.hpp"
#include "core/pool.hpp"

namespace hft::book {

template <class IdMap = LinearMap>
class TickBook {
   public:
    static constexpr std::uint32_t kLevels = 2048;

    explicit TickBook(std::size_t expected_orders = 1024)
        : hot_(expected_orders),
          cold_(expected_orders),
          ids_(expected_orders),
          over_{Pool<OverLevel>(64), Pool<OverLevel>(64)} {
        clear();
    }

    void clear() {
        ids_.clear();
        used_ = 0;
        free_ = kNoOrder;
        live_ = 0;
        tick_ = 0;
        base_ = 0;
        recentres_ = 0;
        n_over_[0] = n_over_[1] = 0;
        summary_[0] = summary_[1] = 0;
        std::memset(bits_, 0, sizeof bits_);
        for (auto& side : win_)
            for (Level& l : side) l = Level{};
    }

    bool add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price,
             std::uint64_t seq) {
        if (shares == 0 || !valid_price(price) || ids_.find(ref) != kNoOrder) return false;
        place(ref, static_cast<int>(side), shares, price, seq);
        return true;
    }

    bool execute(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }
    bool cancel(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }

    bool erase(std::uint64_t ref) {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder) return false;
        remove(i, ref);
        return true;
    }

    bool replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                 std::uint32_t price, std::uint64_t seq) {
        const std::uint32_t i = ids_.find(old_ref);
        if (i == kNoOrder || shares == 0 || !valid_price(price)) return false;
        if (new_ref != old_ref && ids_.find(new_ref) != kNoOrder) return false;
        const int s = side_of(hot_[i]);
        remove(i, old_ref);
        place(new_ref, s, shares, price, seq);
        return true;
    }

    Bbo bbo() const {
        Bbo b;
        if (const auto [px, l] = best(0); l) b.bid_px = px, b.bid_qty = l->qty;
        if (const auto [px, l] = best(1); l) b.ask_px = px, b.ask_qty = l->qty;
        return b;
    }

    std::size_t order_count() const { return live_; }

    std::uint32_t shares(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        return i == kNoOrder ? 0 : hot_[i].qty;
    }

    std::uint64_t resting_shares() const {
        std::uint64_t n = 0;
        for (int s = 0; s < 2; ++s) {
            for (const Level& l : win_[s]) n += l.qty;
            for (std::uint32_t k = 0; k < n_over_[s]; ++k) n += over_[s][k].l.qty;
        }
        return n;
    }
    std::uint64_t recentres() const { return recentres_; }
    std::size_t overflow_levels() const { return n_over_[0] + n_over_[1]; }

    std::int64_t queue_ahead(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder) return -1;
        std::int64_t ahead = 0;
        for (std::uint32_t j = hot_[i].prev; j != kNoOrder; j = hot_[j].prev) ahead += hot_[j].qty;
        return ahead;
    }

    // Every level non-empty with quantity equal to the sum of its orders, links consistent,
    // bitmaps matching the levels, overflow sorted and holding only prices the window cannot,
    // and every order reachable through the ID map exactly once.
    bool check() const {
        std::size_t n = 0;
        auto level_ok = [&](const Level& l, int s, std::uint32_t px) {
            if (l.head == kNoOrder || l.tail == kNoOrder) return false;
            std::uint64_t sum = 0;
            std::uint32_t prev = kNoOrder;
            for (std::uint32_t j = l.head; j != kNoOrder; prev = j, j = hot_[j].next) {
                const Hot& h = hot_[j];
                if (h.prev != prev || side_of(h) != s || price_of(h) != px || h.qty == 0)
                    return false;
                if (ids_.find(cold_[j].ref) != j) return false;
                sum += h.qty;
                if (++n > live_) return false;
            }
            return prev == l.tail && sum == l.qty;
        };
        for (int s = 0; s < 2; ++s) {
            for (std::uint32_t w = 0; w < kWords; ++w) {
                if (((summary_[s] >> w) & 1) != (bits_[s][w] != 0)) return false;
                for (std::uint32_t b = 0; b < 64; ++b) {
                    const std::uint32_t k = w * 64 + b;
                    const Level& l = win_[s][k];
                    const bool set = (bits_[s][w] >> b) & 1;
                    if (set != (l.head != kNoOrder)) return false;
                    if (set && !level_ok(l, s, base_ + k * tick_)) return false;
                }
            }
            for (std::uint32_t k = 0; k < n_over_[s]; ++k) {
                const OverLevel& o = over_[s][k];
                if (k && !worse(s, over_[s][k - 1].px, o.px)) return false;
                if (slot(o.px) != kNoOrder || !level_ok(o.l, s, o.px)) return false;
            }
        }
        return n == live_ && ids_.size() == live_;
    }

   private:
    static constexpr std::uint32_t kWords = kLevels / 64;
    static constexpr std::uint32_t kSell = 1u << 31;

    struct Hot {
        std::uint32_t px;  // price, top bit set for sell
        std::uint32_t qty;
        std::uint32_t next;
        std::uint32_t prev;
    };
    static_assert(sizeof(Hot) == 16);
    struct Cold {
        std::uint64_t ref;
        std::uint64_t seq;
    };
    struct Level {
        std::uint64_t qty = 0;
        std::uint32_t head = kNoOrder;
        std::uint32_t tail = kNoOrder;
    };
    struct OverLevel {
        std::uint32_t px;
        std::uint32_t pad;
        Level l;
    };

    static bool valid_price(std::uint32_t px) { return px != 0 && px < kSell; }
    static int side_of(const Hot& h) { return h.px >> 31; }
    static std::uint32_t price_of(const Hot& h) { return h.px & ~kSell; }
    // Bids keep the best (highest) price last in overflow, asks the lowest.
    static bool worse(int s, std::uint32_t a, std::uint32_t b) { return s == 0 ? a < b : a > b; }

    // Window index of a price, or kNoOrder when it belongs in overflow.
    std::uint32_t slot(std::uint32_t px) const {
        if (px < base_ || tick_ == 0) return kNoOrder;
        const std::uint32_t off = px - base_;
        const std::uint32_t k = tick_ == 1 ? off : off / 100;
        if (k >= kLevels || k * tick_ != off) return kNoOrder;
        return k;
    }

    std::pair<std::uint32_t, const Level*> best(int s) const {
        std::uint32_t px = 0;
        const Level* l = nullptr;
        if (summary_[s]) {
            std::uint32_t k;
            if (s == 0) {
                const std::uint32_t w = 63 - std::countl_zero(summary_[s]);
                k = w * 64 + 63 - std::countl_zero(bits_[s][w]);
            } else {
                const std::uint32_t w = std::countr_zero(summary_[s]);
                k = w * 64 + std::countr_zero(bits_[s][w]);
            }
            px = base_ + k * tick_;
            l = &win_[s][k];
        }
        if (n_over_[s]) {
            const OverLevel& o = over_[s][n_over_[s] - 1];
            if (!l || worse(s, px, o.px)) px = o.px, l = &o.l;
        }
        return {px, l};
    }

    OverLevel* over_find(int s, std::uint32_t px) {
        OverLevel* b = &over_[s][0];
        OverLevel* e = b + n_over_[s];
        OverLevel* it = std::lower_bound(
            b, e, px, [s](const OverLevel& o, std::uint32_t p) { return worse(s, o.px, p); });
        return it != e && it->px == px ? it : nullptr;
    }

    Level& over_insert(int s, std::uint32_t px) {
        over_[s].reserve(n_over_[s] + 1);
        OverLevel* b = &over_[s][0];
        OverLevel* e = b + n_over_[s];
        OverLevel* it = std::lower_bound(
            b, e, px, [s](const OverLevel& o, std::uint32_t p) { return worse(s, o.px, p); });
        if (it != e && it->px == px) return it->l;
        std::memmove(it + 1, it, static_cast<std::size_t>(e - it) * sizeof(OverLevel));
        *it = OverLevel{px, 0, Level{}};
        ++n_over_[s];
        return it->l;
    }

    void over_erase(int s, OverLevel* it) {
        OverLevel* e = &over_[s][0] + n_over_[s];
        std::memmove(it, it + 1, static_cast<std::size_t>(e - it - 1) * sizeof(OverLevel));
        --n_over_[s];
    }

    void set_bit(int s, std::uint32_t k) {
        bits_[s][k / 64] |= 1ull << (k % 64);
        summary_[s] |= 1ull << (k / 64);
    }
    void clear_bit(int s, std::uint32_t k) {
        if (!(bits_[s][k / 64] &= ~(1ull << (k % 64)))) summary_[s] &= ~(1ull << (k / 64));
    }

    Level& level_for_add(int s, std::uint32_t px) {
        if (tick_ == 0) {
            tick_ = px >= 1'0000 ? 100 : 1;
            recentre(s, px);
        }
        std::uint32_t k = slot(px);
        if (k == kNoOrder) {
            const auto [bpx, bl] = best(s);
            if (!bl || worse(s, bpx, px)) {
                recentre(s, px);
                k = slot(px);
            }
        }
        if (k == kNoOrder) return over_insert(s, px);
        set_bit(s, k);
        return win_[s][k];
    }

    // Moves the window so that `px` lands in it, centred on the spread when the opposite best
    // is close enough. Orders keep their prices, so only levels move.
    void recentre(int s, std::uint32_t px) {
        const std::uint32_t half = (kLevels / 2) * tick_;
        std::uint32_t centre = px;
        if (const auto [other, ol] = best(1 - s); ol) {
            const std::uint32_t lo = std::min(px, other), hi = std::max(px, other);
            if (hi - lo < half) centre = lo + (hi - lo) / 2;
        }
        const std::uint32_t grid = centre / tick_ * tick_;
        const std::uint32_t new_base = grid - std::min(grid, half);
        if (new_base == base_) return;
        ++recentres_;

        // Window levels go to overflow first, then every overflow level that fits moves in.
        for (int s = 0; s < 2; ++s) {
            while (summary_[s]) {
                const std::uint32_t w = std::countr_zero(summary_[s]);
                const std::uint32_t k = w * 64 + std::countr_zero(bits_[s][w]);
                over_insert(s, base_ + k * tick_) = win_[s][k];
                win_[s][k] = Level{};
                clear_bit(s, k);
            }
        }
        base_ = new_base;
        for (int s = 0; s < 2; ++s) {
            std::uint32_t keep = 0;
            for (std::uint32_t i = 0; i < n_over_[s]; ++i) {
                const OverLevel& o = over_[s][i];
                if (const std::uint32_t k = slot(o.px); k != kNoOrder) {
                    win_[s][k] = o.l;
                    set_bit(s, k);
                } else {
                    over_[s][keep++] = o;
                }
            }
            n_over_[s] = keep;
        }
    }

    Level& level_of(const Hot& h) {
        const int s = side_of(h);
        const std::uint32_t px = price_of(h);
        if (const std::uint32_t k = slot(px); k != kNoOrder) return win_[s][k];
        return over_find(s, px)->l;
    }

    void drop_level_if_empty(const Hot& h) {
        const int s = side_of(h);
        const std::uint32_t px = price_of(h);
        if (const std::uint32_t k = slot(px); k != kNoOrder) {
            if (win_[s][k].head == kNoOrder) clear_bit(s, k);
        } else {
            OverLevel* o = over_find(s, px);
            if (o->l.head == kNoOrder) over_erase(s, o);
        }
    }

    void place(std::uint64_t ref, int s, std::uint32_t shares, std::uint32_t price,
               std::uint64_t seq) {
        std::uint32_t i;
        if (free_ != kNoOrder) {
            i = free_;
            free_ = hot_[i].next;
        } else {
            i = used_++;
            hot_.reserve(used_);
            cold_.reserve(used_);
        }
        Level& l = level_for_add(s, price);
        hot_[i] = Hot{price | (s ? kSell : 0), shares, kNoOrder, l.tail};
        cold_[i] = Cold{ref, seq};
        if (l.tail != kNoOrder)
            hot_[l.tail].next = i;
        else
            l.head = i;
        l.tail = i;
        l.qty += shares;
        ids_.insert(ref, i);
        ++live_;
    }

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder || shares == 0 || shares > hot_[i].qty) return false;
        if (shares == hot_[i].qty) {
            remove(i, ref);
            return true;
        }
        hot_[i].qty -= shares;
        level_of(hot_[i]).qty -= shares;
        return true;
    }

    void remove(std::uint32_t i, std::uint64_t ref) {
        Hot& h = hot_[i];
        Level& l = level_of(h);
        l.qty -= h.qty;
        if (h.prev != kNoOrder)
            hot_[h.prev].next = h.next;
        else
            l.head = h.next;
        if (h.next != kNoOrder)
            hot_[h.next].prev = h.prev;
        else
            l.tail = h.prev;
        drop_level_if_empty(h);
        ids_.erase(ref);
        h.next = free_;
        free_ = i;
        --live_;
    }

    Pool<Hot> hot_;
    Pool<Cold> cold_;
    IdMap ids_;
    Pool<OverLevel> over_[2];
    std::uint32_t n_over_[2];
    std::uint32_t used_, free_;
    std::size_t live_;
    std::uint32_t tick_, base_;
    std::uint64_t recentres_;
    std::uint64_t summary_[2];
    std::uint64_t bits_[2][kWords];
    Level win_[2][kLevels];
};

}  // namespace hft::book
