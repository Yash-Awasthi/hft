#pragma once

// L3 book with price levels in a B-tree (tlx::btree_map) per side, otherwise built like
// TickBook: index-addressed orders and levels and the same order-ID maps. A comparison
// point for the level container only; the B-tree allocates nodes as levels appear.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <tlx/container/btree_map.hpp>

#include "book/id_map.hpp"
#include "book/order_store.hpp"
#include "book/types.hpp"
#include "core/pool.hpp"

namespace hft::book {

template <class IdMap = LinearMap, class Store = HotCold>
class BTreeBook {
   public:
    explicit BTreeBook(std::size_t expected_orders = 1024)
        : o_(expected_orders), ids_(expected_orders), levels_(256) {}

    bool add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price,
             std::uint64_t seq) {
        if (shares == 0 || !valid_price(price) || ref >= kMaxRef || ids_.find(ref) != kNoOrder)
            return false;
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
        if (new_ref >= kMaxRef || (new_ref != old_ref && ids_.find(new_ref) != kNoOrder))
            return false;
        const int s = static_cast<int>(o_.px(i) >> 31);
        remove(i, old_ref);
        place(new_ref, s, shares, price, seq);
        return true;
    }

    Bbo bbo() const {
        Bbo b;
        if (!bids_.empty())
            b.bid_px = bids_.begin()->first, b.bid_qty = levels_[bids_.begin()->second].qty;
        if (!asks_.empty())
            b.ask_px = asks_.begin()->first, b.ask_qty = levels_[asks_.begin()->second].qty;
        return b;
    }
    std::size_t order_count() const { return live_; }
    std::uint32_t shares(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        return i == kNoOrder ? 0 : o_.qty(i);
    }
    std::uint64_t resting_shares() const {
        std::uint64_t n = 0;
        for (const auto& [px, l] : bids_) n += levels_[l].qty;
        for (const auto& [px, l] : asks_) n += levels_[l].qty;
        return n;
    }
    std::int64_t queue_ahead(std::uint64_t ref) const {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder) return -1;
        std::int64_t ahead = 0;
        for (std::uint32_t j = o_.prev(i); j != kNoOrder; j = o_.prev(j)) ahead += o_.qty(j);
        return ahead;
    }

    bool check() const {
        std::size_t n = 0;
        auto side_ok = [&](const auto& m, int s) {
            for (const auto& [px, li] : m) {
                const Level& l = levels_[li];
                if (l.head == kNoOrder) return false;
                std::uint64_t sum = 0;
                std::uint32_t prev = kNoOrder;
                for (std::uint32_t j = l.head; j != kNoOrder; prev = j, j = o_.next(j)) {
                    if (o_.prev(j) != prev || o_.px(j) != (px | (s ? kSell : 0)) || o_.qty(j) == 0)
                        return false;
                    if (ids_.find(o_.ref(j)) != j) return false;
                    sum += o_.qty(j);
                    if (++n > live_) return false;
                }
                if (prev != l.tail || sum != l.qty) return false;
            }
            return true;
        };
        return side_ok(bids_, 0) && side_ok(asks_, 1) && n == live_ && ids_.size() == live_;
    }

   private:
    static constexpr std::uint32_t kSell = 1u << 31;
    struct Level {
        std::uint64_t qty;
        std::uint32_t head, tail;
    };
    static bool valid_price(std::uint32_t px) { return px != 0 && px < kSell; }

    std::uint32_t level_index(int s, std::uint32_t px) const {
        return s ? asks_.find(px)->second : bids_.find(px)->second;
    }

    std::uint32_t level_for_add(int s, std::uint32_t px) {
        auto get = [&](auto& m) {
            auto it = m.find(px);
            if (it != m.end()) return it->second;
            std::uint32_t li;
            if (free_level_ != kNoOrder) {
                li = free_level_;
                free_level_ = levels_[li].head;
            } else {
                li = used_levels_++;
                levels_.reserve(used_levels_);
            }
            levels_[li] = Level{0, kNoOrder, kNoOrder};
            m.insert(std::make_pair(px, li));
            return li;
        };
        return s ? get(asks_) : get(bids_);
    }

    void place(std::uint64_t ref, int s, std::uint32_t shares, std::uint32_t price,
               std::uint64_t seq) {
        std::uint32_t i;
        if (free_ != kNoOrder) {
            i = free_;
            free_ = o_.next(i);
        } else {
            if (used_ == kMaxOrders) throw std::length_error("book holds 2^24 orders");
            i = used_++;
            o_.reserve(used_);
        }
        Level& l = levels_[level_for_add(s, price)];
        o_.set(i, price | (s ? kSell : 0), shares, kNoOrder, l.tail, ref, seq);
        if (l.tail != kNoOrder)
            o_.next(l.tail) = i;
        else
            l.head = i;
        l.tail = i;
        l.qty += shares;
        ids_.insert(ref, i);
        ++live_;
    }

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNoOrder || shares == 0 || shares > o_.qty(i)) return false;
        if (shares == o_.qty(i)) {
            remove(i, ref);
            return true;
        }
        o_.qty(i) -= shares;
        const std::uint32_t px = o_.px(i);
        levels_[level_index(static_cast<int>(px >> 31), px & ~kSell)].qty -= shares;
        return true;
    }

    void remove(std::uint32_t i, std::uint64_t ref) {
        const std::uint32_t stored = o_.px(i), next = o_.next(i), prev = o_.prev(i);
        const int s = static_cast<int>(stored >> 31);
        const std::uint32_t px = stored & ~kSell;
        const std::uint32_t li = level_index(s, px);
        Level& l = levels_[li];
        l.qty -= o_.qty(i);
        if (prev != kNoOrder)
            o_.next(prev) = next;
        else
            l.head = next;
        if (next != kNoOrder)
            o_.prev(next) = prev;
        else
            l.tail = prev;
        if (l.head == kNoOrder) {
            if (s)
                asks_.erase(px);
            else
                bids_.erase(px);
            l.head = free_level_;
            free_level_ = li;
        }
        ids_.erase(ref);
        o_.next(i) = free_;
        free_ = i;
        --live_;
    }

    Store o_;
    IdMap ids_;
    Pool<Level> levels_;
    tlx::btree_map<std::uint32_t, std::uint32_t, std::greater<std::uint32_t>> bids_;
    tlx::btree_map<std::uint32_t, std::uint32_t> asks_;
    std::uint32_t used_ = 0, free_ = kNoOrder;
    std::uint32_t used_levels_ = 0, free_level_ = kNoOrder;
    std::size_t live_ = 0;
};

}  // namespace hft::book
