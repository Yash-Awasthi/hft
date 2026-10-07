#pragma once

// Baseline L3 book on std::map and std::list. Slow, simple, and the reference every other
// implementation is compared against.

#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "book/types.hpp"

namespace hft::book {

class MapBook {
   public:
    bool add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price,
             std::uint64_t seq, std::uint32_t = 0) {
        if (shares == 0 || !valid_price(price) || ref >= kMaxRef || orders_.contains(ref))
            return false;
        Level& l = side == Side::Buy ? bids_[price] : asks_[price];
        l.qty += shares;
        l.queue.push_back(ref);
        orders_.emplace(ref, Order{side, price, shares, seq, std::prev(l.queue.end())});
        return true;
    }

    bool execute(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }
    bool cancel(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }

    bool erase(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        remove(it);
        return true;
    }

    bool replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                 std::uint32_t price, std::uint64_t seq) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end() || shares == 0 || !valid_price(price)) return false;
        if (new_ref >= kMaxRef || (new_ref != old_ref && orders_.contains(new_ref))) return false;
        const Side side = it->second.side;
        remove(it);
        return add(new_ref, side, shares, price, seq);
    }

    Bbo bbo() const {
        Bbo b;
        if (!bids_.empty()) b.bid_px = bids_.begin()->first, b.bid_qty = bids_.begin()->second.qty;
        if (!asks_.empty()) b.ask_px = asks_.begin()->first, b.ask_qty = asks_.begin()->second.qty;
        return b;
    }

    std::size_t order_count() const { return orders_.size(); }

    // Remaining shares of an order, 0 if unknown.
    std::uint32_t shares(std::uint64_t ref) const {
        auto it = orders_.find(ref);
        return it == orders_.end() ? 0 : it->second.shares;
    }

    std::uint64_t resting_shares() const {
        std::uint64_t n = 0;
        for (const auto& [px, l] : bids_) n += l.qty;
        for (const auto& [px, l] : asks_) n += l.qty;
        return n;
    }

    // Level quantity equals the sum of its orders, every order is indexed by its ID and sits
    // on the level of its price and side, and no level is empty.
    bool check() const {
        std::size_t n = 0;
        auto side_ok = [&](const auto& levels, Side side) {
            for (const auto& [px, l] : levels) {
                if (l.queue.empty()) return false;
                std::uint64_t sum = 0;
                for (std::uint64_t ref : l.queue) {
                    auto it = orders_.find(ref);
                    if (it == orders_.end()) return false;
                    const Order& o = it->second;
                    if (o.side != side || o.price != px || o.shares == 0) return false;
                    sum += o.shares;
                    ++n;
                }
                if (sum != l.qty) return false;
            }
            return true;
        };
        return side_ok(bids_, Side::Buy) && side_ok(asks_, Side::Sell) && n == orders_.size();
    }

    // Every level of a side, best first, as (price, shares).
    std::vector<std::pair<std::uint32_t, std::uint64_t>> depth(Side side) const {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
        if (side == Side::Buy)
            for (const auto& [px, l] : bids_) out.emplace_back(px, l.qty);
        else
            for (const auto& [px, l] : asks_) out.emplace_back(px, l.qty);
        return out;
    }

    // Shares resting ahead of `ref` at its level; -1 if unknown.
    std::int64_t queue_ahead(std::uint64_t ref) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return -1;
        const Order& o = it->second;
        const Level& l = o.side == Side::Buy ? bids_.at(o.price) : asks_.at(o.price);
        std::int64_t ahead = 0;
        for (auto q = l.queue.begin(); q != o.pos; ++q) ahead += orders_.at(*q).shares;
        return ahead;
    }

   private:
    // Zero marks an empty side in Bbo; the top bit is free for a side flag in packed books.
    static bool valid_price(std::uint32_t px) { return px != 0 && px < (1u << 31); }

    struct Level {
        std::uint64_t qty = 0;
        std::list<std::uint64_t> queue;
    };
    struct Order {
        Side side;
        std::uint32_t price;
        std::uint32_t shares;
        std::uint64_t seq;
        std::list<std::uint64_t>::iterator pos;
    };
    using Orders = std::unordered_map<std::uint64_t, Order>;

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end() || shares == 0 || shares > it->second.shares) return false;
        if (shares == it->second.shares) {
            remove(it);
            return true;
        }
        Order& o = it->second;
        o.shares -= shares;
        (o.side == Side::Buy ? bids_.at(o.price) : asks_.at(o.price)).qty -= shares;
        return true;
    }

    void remove(Orders::iterator it) {
        const Order& o = it->second;
        auto drop = [&](auto& levels) {
            auto l = levels.find(o.price);
            l->second.qty -= o.shares;
            l->second.queue.erase(o.pos);
            if (l->second.queue.empty()) levels.erase(l);
        };
        if (o.side == Side::Buy)
            drop(bids_);
        else
            drop(asks_);
        orders_.erase(it);
    }

    std::map<std::uint32_t, Level, std::greater<>> bids_;
    std::map<std::uint32_t, Level> asks_;
    Orders orders_;
};

}  // namespace hft::book
