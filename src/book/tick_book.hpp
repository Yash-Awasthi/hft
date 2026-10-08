#pragma once

// Main L3 book. Price levels live in a dense window of kLevels ticks per side with a
// two-level bitmap of non-empty levels, so the best level is two lzcnt or tzcnt. Levels on
// the tick grid but outside the window go to a radix tree (level_radix.hpp); the rare
// prices off the grid sit in a small sorted array. Orders are index-addressed in the layout
// chosen by Store (order_store.hpp).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

#include "book/id_map.hpp"
#include "book/level_radix.hpp"
#include "book/order_store.hpp"
#include "book/types.hpp"
#include "core/pool.hpp"

namespace hft::book {

// Levels == 0 gives a sorted-vector book: every level lives in the sorted overflow arrays.
template <class IdMap = LinearMap, class Store = HotCold, std::uint32_t Levels = 2048>
class TickBook {
   public:
    static constexpr std::uint32_t kLevels = Levels;

    // tick: price grid in ITCH units (100 is $0.01, 50 is $0.005); 0 picks 100 or, for a first
    // price under $1, 1.
    explicit TickBook(std::size_t expected_orders = 1024, std::uint32_t tick = 0)
        : o_(expected_orders),
          ids_(expected_orders),
          over_{Pool<OverLevel>(64), Pool<OverLevel>(64)},
          fixed_tick_(tick) {
        clear();
    }

    void clear() {
        ids_.clear();
        used_ = 0;
        free_ = kNoOrder;
        live_ = 0;
        tick_ = fixed_tick_;
        centred_ = false;
        base_ = 0;
        base_g_ = 0;
        recentres_ = 0;
        n_over_[0] = n_over_[1] = 0;
        deep_[0].clear();
        deep_[1].clear();
        summary_[0] = summary_[1] = 0;
        std::memset(bits_, 0, sizeof bits_);
        for (auto& side : win_)
            for (Level& l : side) l = Level{};
    }

    bool add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price,
             std::uint64_t seq, std::uint32_t owner = 0) {
        if (shares == 0 || !valid_price(price) || ref >= kMaxRef) return false;
        const IdProbe at = ids_.probe(ref);
        if (at.val != kNoOrder) return false;
        place(ref, static_cast<int>(side), shares, price, seq, owner, at);
        return true;
    }

    bool execute(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }
    bool cancel(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }

    bool erase(std::uint64_t ref) {
        const IdProbe at = ids_.probe(ref);
        if (at.val == kNoOrder) return false;
        remove(at);
        return true;
    }

    bool replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                 std::uint32_t price, std::uint64_t seq) {
        const IdProbe old = ids_.probe(old_ref);
        if (old.val == kNoOrder || shares == 0 || !valid_price(price) || new_ref >= kMaxRef)
            return false;
        IdProbe at{0, kNoOrder};
        if (new_ref != old_ref && (at = ids_.probe(new_ref)).val != kNoOrder) return false;
        const int s = side_of(o_.px(old.val));
        const std::uint32_t owner = o_.owner(old.val);
        const std::size_t hole = remove(old);
        at = new_ref == old_ref ? ids_.probe(new_ref) : ids_.after_erase(at, new_ref, hole);
        place(new_ref, s, shares, price, seq, owner, at);
        return true;
    }

    // Replay knows upcoming events: warm the ID slot early, then the order record once the
    // slot is cached. Neither changes state.
    void prefetch_id(std::uint64_t ref) const { ids_.prefetch(ref); }
    void prefetch_order(std::uint64_t ref) const {
        if (const std::uint32_t i = ids_.find(ref); i != kNoOrder) o_.prefetch(i);
    }

    Bbo bbo() const {
        Bbo b;
        if (const auto [px, l] = best(0); l) b.bid_px = px, b.bid_qty = l->qty;
        if (const auto [px, l] = best(1); l) b.ask_px = px, b.ask_qty = l->qty;
        return b;
    }

    std::size_t order_count() const { return live_; }

    // First order in time priority at the best level of a side.
    std::optional<OrderView> front(Side side) const {
        const auto [px, l] = best(static_cast<int>(side));
        if (!l) return std::nullopt;
        return view(l->head);
    }
    // The order queued directly behind `ref` at its level.
    std::optional<OrderView> behind(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder || o_.next(i) == kNoOrder) return std::nullopt;
        return view(o_.next(i));
    }
    // Head order of the nearest level on `side` strictly worse than `px`.
    std::optional<OrderView> next_level(Side side, std::uint32_t px) const {
        const auto [p, l] = next_level_raw(side, px);
        if (!l) return std::nullopt;
        return view(l->head);
    }

    // Price and total shares of a level, for callers that do not need its orders: no order
    // record is read.
    struct LevelInfo {
        std::uint32_t price;
        std::uint64_t qty;
    };
    std::optional<LevelInfo> best_level(Side side) const {
        const auto [px, l] = best(static_cast<int>(side));
        if (!l) return std::nullopt;
        return LevelInfo{px, l->qty};
    }
    std::optional<LevelInfo> next_level_info(Side side, std::uint32_t px) const {
        const auto [p, l] = next_level_raw(side, px);
        if (!l) return std::nullopt;
        return LevelInfo{p, l->qty};
    }

    // Sum of the shares of the orders at (side, price), in queue order, for which
    // pred(ref, seq) holds. Walks the level by index; zero when the level is absent.
    template <class Pred>
    std::uint64_t sum_where(Side side, std::uint32_t price, Pred&& pred) const {
        const Level* l = find_level(static_cast<int>(side), price);
        std::uint64_t n = 0;
        if (l)
            for (std::uint32_t i = l->head; i != kNoOrder; i = o_.next(i))
                if (pred(o_.ref(i), o_.seq(i))) n += o_.qty(i);
        return n;
    }

    // Shares resting at one price on one side.
    std::uint64_t level_qty(Side side, std::uint32_t px) const {
        const int s = static_cast<int>(side);
        if (const std::uint32_t k = slot(px); k != kNoOrder)
            return (bits_[s][k / 64] >> (k % 64)) & 1 ? win_[s][k].qty : 0;
        if (const std::uint32_t g = grid(px); g != kNoOrder) {
            const Level* l = deep_[s].find(g);
            return l ? l->qty : 0;
        }
        const OverLevel* b = &over_[s][0];
        const OverLevel* e = b + n_over_[s];
        const OverLevel* it = std::lower_bound(
            b, e, px, [s](const OverLevel& o, std::uint32_t p) { return worse(s, o.px, p); });
        return it != e && it->px == px ? it->l.qty : 0;
    }

    std::optional<OrderView> order(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder) return std::nullopt;
        return view(i);
    }

    std::uint32_t shares(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        return i == kNoOrder ? 0 : o_.qty(i);
    }

    std::uint64_t resting_shares() const {
        std::uint64_t n = 0;
        for (int s = 0; s < 2; ++s) {
            for (const Level& l : win_[s]) n += l.qty;
            for (std::uint32_t k = 0; k < n_over_[s]; ++k) n += over_[s][k].l.qty;
            deep_[s].for_each([&](std::uint32_t, const Level& l) { n += l.qty; });
        }
        return n;
    }
    std::uint64_t recentres() const { return recentres_; }
    std::size_t overflow_levels() const {
        return n_over_[0] + n_over_[1] + deep_[0].count() + deep_[1].count();
    }

    std::int64_t queue_ahead(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder) return -1;
        std::int64_t ahead = 0;
        for (std::uint32_t j = o_.prev(i); j != kNoOrder; j = o_.prev(j)) ahead += o_.qty(j);
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
            for (std::uint32_t j = l.head; j != kNoOrder; prev = j, j = o_.next(j)) {
                if (o_.prev(j) != prev || side_of(o_.px(j)) != s || price_of(o_.px(j)) != px ||
                    o_.qty(j) == 0)
                    return false;
                if (ids_.find(o_.ref(j)) != j) return false;
                sum += o_.qty(j);
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
                if (grid(o.px) != kNoOrder || !level_ok(o.l, s, o.px)) return false;
            }
            bool deep_ok = true;
            deep_[s].for_each([&](std::uint32_t g, const Level& l) {
                deep_ok = deep_ok && slot(g * tick_) == kNoOrder && level_ok(l, s, g * tick_);
            });
            if (!deep_ok) return false;
        }
        return n == live_ && ids_.size() == live_;
    }

    // Fork: copies the used part of every pool and the fixed-size state. Indices replace
    // pointers throughout, so the copy needs no fix-up.
    void copy_from(const TickBook& o) {
        o_.copy_from(o.o_, o.used_);
        ids_.copy_from(o.ids_);
        for (int s = 0; s < 2; ++s) {
            over_[s].copy_from(o.over_[s], o.n_over_[s]);
            n_over_[s] = o.n_over_[s];
            deep_[s].copy_from(o.deep_[s]);
            summary_[s] = o.summary_[s];
        }
        used_ = o.used_;
        free_ = o.free_;
        live_ = o.live_;
        tick_ = o.tick_;
        centred_ = o.centred_;
        base_ = o.base_;
        base_g_ = o.base_g_;
        recentres_ = o.recentres_;
        std::memcpy(bits_, o.bits_, sizeof bits_);
        std::memcpy(win_, o.win_, sizeof win_);
    }

    // Bytes a fork copies.
    std::size_t state_bytes() const {
        std::vector<std::uint8_t> img;
        save(img);
        return img.size();
    }

    // Raw image of the whole state (pools up to their used size, window, ID table). It is a
    // checkpoint format, not an interchange one: valid only for the same build of this type.
    void save(std::vector<std::uint8_t>& out) const {
        out.clear();
        const Header h{kMagic,
                       used_,
                       free_,
                       static_cast<std::uint64_t>(live_),
                       tick_,
                       base_,
                       static_cast<std::uint32_t>(recentres_),
                       centred_,
                       {n_over_[0], n_over_[1]},
                       {summary_[0], summary_[1]}};
        detail::put(out, &h, sizeof h);
        detail::put(out, bits_, sizeof bits_);
        detail::put(out, win_, sizeof win_);
        o_.save(out, used_);
        for (int s = 0; s < 2; ++s) {
            detail::put(out, over_[s].data(), n_over_[s] * sizeof(OverLevel));
            deep_[s].save(out);
        }
        ids_.save(out);
    }

    // Returns false and leaves the book unspecified on a malformed image.
    bool load(const std::uint8_t* p, std::size_t n) {
        const std::uint8_t* end = p + n;
        Header h;
        if (!detail::get(p, end, &h, sizeof h) || h.magic != kMagic || h.live > h.used)
            return false;
        if (!detail::get(p, end, bits_, sizeof bits_) || !detail::get(p, end, win_, sizeof win_))
            return false;
        if (!o_.load(p, end, h.used)) return false;
        for (int s = 0; s < 2; ++s) {
            over_[s].reserve(h.n_over[s]);
            if (!detail::get(p, end, over_[s].data(), h.n_over[s] * sizeof(OverLevel)))
                return false;
            n_over_[s] = h.n_over[s];
            summary_[s] = h.summary[s];
            if (!deep_[s].load(p, end)) return false;
        }
        if (!ids_.load(p, end) || p != end) return false;
        used_ = h.used;
        free_ = h.free;
        live_ = h.live;
        tick_ = h.tick;
        centred_ = h.centred != 0;
        base_ = h.base;
        base_g_ = tick_ ? base_ / tick_ : 0;
        recentres_ = h.recentres;
        return true;
    }

   private:
    static constexpr std::uint64_t kMagic = 0x334b4f4f424b4354;  // "TCKBOOK3"
    struct Header {
        std::uint64_t magic;
        std::uint32_t used, free;
        std::uint64_t live;
        std::uint32_t tick, base;
        std::uint32_t recentres, centred;
        std::uint32_t n_over[2];
        std::uint64_t summary[2];
    };

    static constexpr std::uint32_t kWords = kLevels / 64;
    static constexpr std::uint32_t kSell = 1u << 31;

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
    OrderView view(std::uint32_t i) const {
        const std::uint32_t px = o_.px(i);
        return {o_.ref(i), o_.seq(i),   price_of(px),
                o_.qty(i), o_.owner(i), static_cast<Side>(side_of(px))};
    }

    // Stored prices carry the side in the top bit.
    static int side_of(std::uint32_t px) { return px >> 31; }
    static std::uint32_t price_of(std::uint32_t px) { return px & ~kSell; }
    // Bids keep the best (highest) price last in overflow, asks the lowest.
    static bool worse(int s, std::uint32_t a, std::uint32_t b) { return s == 0 ? a < b : a > b; }

    // Tick index of a price for the radix, or kNoOrder when off the grid or out of its range.
    std::uint32_t grid(std::uint32_t px) const {
        if (kLevels == 0 || tick_ == 0) return kNoOrder;  // the sorted-vector book has no radix
        const std::uint32_t g = tick_ == 1 ? px : tick_ == 100 ? px / 100 : px / tick_;
        return g * tick_ == px && g < LevelRadix<Level>::kRange ? g : kNoOrder;
    }

    // Window index of a price, or kNoOrder when it belongs in overflow. Unsigned wrap sends
    // indices below the window out of range too.
    std::uint32_t slot(std::uint32_t px) const {
        const std::uint32_t g = grid(px);
        return g != kNoOrder && g - base_g_ < kLevels ? g - base_g_ : kNoOrder;
    }

    // The nearest level on `side` strictly worse than `px`: its price and a pointer to it.
    std::pair<std::uint32_t, const Level*> next_level_raw(Side side, std::uint32_t px) const {
        const int s = static_cast<int>(side);
        std::uint32_t best_px = 0;
        const Level* best_l = nullptr;
        auto consider = [&](std::uint32_t p, const Level* l) {
            if (!best_l || worse(s, best_px, p)) best_px = p, best_l = l;
        };
        if (tick_) {
            // Window: indices whose price is strictly worse than px.
            if (s == 0) {
                if (px > base_) {
                    const std::uint64_t lim = (std::uint64_t{px} - base_ + tick_ - 1) / tick_;
                    if (std::uint32_t k; win_below(
                            s, static_cast<std::uint32_t>(std::min<std::uint64_t>(lim, kLevels)),
                            k))
                        consider(base_ + k * tick_, &win_[s][k]);
                }
            } else {
                const std::uint64_t from = px < base_ ? 0 : (std::uint64_t{px} - base_) / tick_ + 1;
                if (std::uint32_t k;
                    from < kLevels && win_from(s, static_cast<std::uint32_t>(from), k))
                    consider(base_ + k * tick_, &win_[s][k]);
            }
            // Radix, on tick indices.
            if (kLevels) {
                std::uint32_t g;
                const std::uint64_t ceil = (std::uint64_t{px} + tick_ - 1) / tick_;
                const bool found =
                    s == 0 ? (ceil >= LevelRadix<Level>::kRange
                                  ? deep_[s].extreme(true, g)
                                  : deep_[s].next(true, static_cast<std::uint32_t>(ceil), g))
                           : px / tick_ < LevelRadix<Level>::kRange &&
                                 deep_[s].next(false, px / tick_, g);
                if (found) consider(g * tick_, &deep_[s].at(g));
            }
        }
        // Off-grid array, worst first: the last entry strictly worse than px.
        const OverLevel* b = &over_[s][0];
        const OverLevel* e = b + n_over_[s];
        const OverLevel* it = std::lower_bound(
            b, e, px, [s](const OverLevel& o, std::uint32_t p) { return worse(s, o.px, p); });
        if (it != b) consider((it - 1)->px, &(it - 1)->l);
        return {best_px, best_l};
    }

    const Level* find_level(int s, std::uint32_t px) const {
        if (const std::uint32_t k = slot(px); k != kNoOrder)
            return (bits_[s][k / 64] >> (k % 64)) & 1 ? &win_[s][k] : nullptr;
        if (const std::uint32_t g = grid(px); g != kNoOrder) return deep_[s].find(g);
        const OverLevel* b = &over_[s][0];
        const OverLevel* e = b + n_over_[s];
        const OverLevel* it = std::lower_bound(
            b, e, px, [s](const OverLevel& o, std::uint32_t p) { return worse(s, o.px, p); });
        return it != e && it->px == px ? &it->l : nullptr;
    }

    // Where a level lives, found once per operation: window slot, radix index or array.
    enum class Where : std::uint8_t { Window, Radix, Array };
    struct Loc {
        Level* l;
        std::uint32_t at;  // window slot, tick index or price
        Where where;
    };

    Loc locate(std::uint32_t stored) {
        const int s = side_of(stored);
        const std::uint32_t px = price_of(stored);
        if (const std::uint32_t g = grid(px); g != kNoOrder) {
            if (const std::uint32_t k = g - base_g_; k < kLevels)
                return {&win_[s][k], k, Where::Window};
            return {deep_[s].find(g), g, Where::Radix};
        }
        return {&over_find(s, px)->l, px, Where::Array};
    }

    void drop_if_empty(int s, const Loc& at) {
        if (at.l->head != kNoOrder) return;
        if (at.where == Where::Window)
            clear_bit(s, at.at);
        else if (at.where == Where::Radix)
            deep_[s].erase(at.at);
        else
            over_erase(s, over_find(s, at.at));
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
        if (std::uint32_t g; deep_[s].extreme(s == 0, g) && (!l || worse(s, px, g * tick_)))
            px = g * tick_, l = &deep_[s].at(g);
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

    // Highest non-empty window index below k, and lowest at or above k.
    bool win_below(int s, std::uint32_t k, std::uint32_t& out) const {
        if (k == 0) return false;
        std::uint32_t w = (k - 1) / 64;
        std::uint64_t x = bits_[s][w] & (~0ull >> (63 - (k - 1) % 64));
        if (!x) {
            const std::uint64_t below = summary_[s] & ((1ull << w) - 1);
            if (!below) return false;
            w = 63 - static_cast<std::uint32_t>(std::countl_zero(below));
            x = bits_[s][w];
        }
        out = w * 64 + 63 - static_cast<std::uint32_t>(std::countl_zero(x));
        return true;
    }
    bool win_from(int s, std::uint32_t k, std::uint32_t& out) const {
        std::uint32_t w = k / 64;
        std::uint64_t x = bits_[s][w] & (~0ull << (k % 64));
        if (!x) {
            const std::uint64_t above = w + 1 < 64 ? summary_[s] & (~0ull << (w + 1)) : 0;
            if (!above) return false;
            w = static_cast<std::uint32_t>(std::countr_zero(above));
            x = bits_[s][w];
        }
        out = w * 64 + static_cast<std::uint32_t>(std::countr_zero(x));
        return true;
    }

    void set_bit(int s, std::uint32_t k) {
        bits_[s][k / 64] |= 1ull << (k % 64);
        summary_[s] |= 1ull << (k / 64);
    }
    void clear_bit(int s, std::uint32_t k) {
        if (!(bits_[s][k / 64] &= ~(1ull << (k % 64)))) summary_[s] &= ~(1ull << (k / 64));
    }

    Level& level_for_add(int s, std::uint32_t px) {
        if (!centred_) {
            if (tick_ == 0) tick_ = px >= 1'0000 ? 100 : 1;
            centred_ = true;
            recentre(s, px);
        }
        const std::uint32_t g = grid(px);
        std::uint32_t k = g - base_g_;
        if (kLevels && (g == kNoOrder || k >= kLevels)) {
            const auto [bpx, bl] = best(s);
            if (!bl || worse(s, bpx, px)) {
                recentre(s, px);
                k = g - base_g_;
            }
        }
        if (g == kNoOrder) return over_insert(s, px);
        if (k >= kLevels) return deep_[s].get(g);
        set_bit(s, k);
        return win_[s][k];
    }

    Level& over_get(int s, std::uint32_t px) {
        if (const std::uint32_t g = grid(px); g != kNoOrder) return deep_[s].get(g);
        return over_insert(s, px);
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
        const std::uint32_t aligned = centre / tick_ * tick_;
        const std::uint32_t new_base = aligned - std::min(aligned, half);
        if (new_base == base_) return;
        ++recentres_;

        // Window levels go to overflow first, then every overflow level that fits moves in.
        for (int s = 0; s < 2; ++s) {
            while (summary_[s]) {
                const std::uint32_t w = std::countr_zero(summary_[s]);
                const std::uint32_t k = w * 64 + std::countr_zero(bits_[s][w]);
                over_get(s, base_ + k * tick_) = win_[s][k];
                win_[s][k] = Level{};
                clear_bit(s, k);
            }
        }
        base_ = new_base;
        base_g_ = base_ / tick_;
        for (int s = 0; s < 2; ++s) {
            for (std::uint32_t k = 0; k < kLevels; ++k) {
                const std::uint32_t g = grid(base_ + k * tick_);
                if (g == kNoOrder) continue;
                if (Level* l = deep_[s].find(g)) {
                    win_[s][k] = *l;
                    set_bit(s, k);
                    deep_[s].erase(g);
                }
            }
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

    void place(std::uint64_t ref, int s, std::uint32_t shares, std::uint32_t price,
               std::uint64_t seq, std::uint32_t owner, IdProbe at) {
        std::uint32_t i;
        if (free_ != kNoOrder) {
            i = free_;
            free_ = o_.next(i);
        } else {
            if (used_ == kMaxOrders) throw std::length_error("book holds 2^24 orders");
            i = used_++;
            o_.reserve(used_);
        }
        Level& l = level_for_add(s, price);
        o_.set(i, price | (s ? kSell : 0), shares, kNoOrder, l.tail, ref, seq, owner);
        if (l.tail != kNoOrder)
            o_.next(l.tail) = i;
        else
            l.head = i;
        l.tail = i;
        l.qty += shares;
        ids_.insert_at(at, ref, i);
        ++live_;
    }

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        const IdProbe at = ids_.probe(ref);
        const std::uint32_t i = at.val;
        if (i == kNoOrder || shares == 0 || shares > o_.qty(i)) return false;
        if (shares == o_.qty(i)) {
            remove(at);
            return true;
        }
        o_.qty(i) -= shares;
        locate(o_.px(i)).l->qty -= shares;
        return true;
    }

    // Returns the ID slot left empty, as erase_at does.
    std::size_t remove(IdProbe id) {
        const std::uint32_t i = id.val;
        const std::uint32_t stored = o_.px(i), next = o_.next(i), prev = o_.prev(i);
        const Loc at = locate(stored);
        Level& l = *at.l;
        l.qty -= o_.qty(i);
        if (prev != kNoOrder)
            o_.next(prev) = next;
        else
            l.head = next;
        if (next != kNoOrder)
            o_.prev(next) = prev;
        else
            l.tail = prev;
        drop_if_empty(side_of(stored), at);
        const std::size_t hole = ids_.erase_at(id.at);
        o_.next(i) = free_;
        free_ = i;
        --live_;
        return hole;
    }

    Store o_;
    IdMap ids_;
    Pool<OverLevel> over_[2];
    LevelRadix<Level> deep_[2];
    std::uint32_t n_over_[2];
    std::uint32_t used_, free_;
    std::size_t live_;
    std::uint32_t fixed_tick_;
    bool centred_;
    std::uint32_t tick_, base_, base_g_;
    std::uint64_t recentres_;
    std::uint64_t summary_[2];
    std::uint64_t bits_[2][kWords ? kWords : 1];
    Level win_[2][kLevels ? kLevels : 1];
};

template <class IdMap = LinearMap, class Store = HotCold>
using SortedVecBook = TickBook<IdMap, Store, 0>;

}  // namespace hft::book
