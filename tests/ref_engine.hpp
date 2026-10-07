#pragma once

// Deliberately naive reference for MatchingEngine: resting orders in a vector, best order
// found by a full scan on every step. Same event semantics, written separately.

#include <cstdint>
#include <optional>
#include <vector>

#include "engine/matching.hpp"

namespace ref {

using namespace hft::engine;

class RefEngine {
   public:
    explicit RefEngine(Config cfg) : cfg_(cfg) {}

    template <class Sink>
    std::uint64_t submit(NewOrder o, Sink&& out) {
        auto reject = [&](Reason r) {
            out(Event{EventType::Rejected, r, o.side, false, o.owner, 0, o.price, o.qty});
            return std::uint64_t{0};
        };
        if (o.qty == 0) return reject(Reason::BadQty);
        if (o.price == 0) {
            if (o.post_only || o.tif != Tif::Ioc) return reject(Reason::BadPrice);
        } else if (o.price % cfg_.tick != 0 || o.price >= (1u << 31)) {
            return reject(Reason::BadPrice);
        }
        const bool buy = o.side == Side::Buy;
        if (o.post_only) {
            if (auto b = best(!buy)) {
                const std::uint32_t opp = rest_[*b].price;
                const bool trades = buy ? o.price >= opp : o.price <= opp;
                if (trades) {
                    if (cfg_.lock == LockPolicy::Reject)
                        return reject(o.price == opp ? Reason::WouldLock : Reason::WouldCross);
                    if (cfg_.lock == LockPolicy::AllowLock) {
                        o.price = opp;
                    } else {
                        if (buy && opp <= cfg_.tick) return reject(Reason::BadPrice);
                        o.price = buy ? opp - cfg_.tick : opp + cfg_.tick;
                    }
                }
            }
        }
        const std::uint64_t ref = next_ref_++;
        out(Event{EventType::Accepted, Reason::None, o.side, false, o.owner, ref, o.price, o.qty});
        std::uint32_t left = o.qty;
        while (left > 0 && !o.post_only) {
            auto b = best(!buy);
            if (!b) break;
            Resting& r = rest_[*b];
            if (o.price != 0 && (buy ? o.price < r.price : o.price > r.price)) break;
            if (cfg_.stp != Stp::None && r.owner == o.owner && o.owner != 0) {
                if (cfg_.stp == Stp::CancelOldest || cfg_.stp == Stp::CancelBoth) {
                    out(Event{EventType::Cancelled, Reason::SelfTrade, r.side, false, r.owner,
                              r.ref, r.price, r.qty});
                    rest_.erase(rest_.begin() + static_cast<std::ptrdiff_t>(*b));
                    if (cfg_.stp == Stp::CancelOldest) continue;
                }
                out(Event{EventType::Cancelled, Reason::SelfTrade, o.side, false, o.owner, ref,
                          o.price, left});
                return ref;
            }
            const std::uint32_t q = left < r.qty ? left : r.qty;
            ++match_;
            out(Event{EventType::Fill, Reason::None, r.side, true, r.owner, r.ref, r.price, q,
                      -cfg_.maker_rebate * q, match_});
            out(Event{EventType::Fill, Reason::None, o.side, false, o.owner, ref, r.price, q,
                      cfg_.taker_fee * q, match_});
            left -= q;
            r.qty -= q;
            if (r.qty == 0) rest_.erase(rest_.begin() + static_cast<std::ptrdiff_t>(*b));
        }
        if (left == 0) return ref;
        if (o.tif == Tif::Ioc) {
            out(Event{EventType::Cancelled, Reason::Ioc, o.side, false, o.owner, ref, o.price,
                      left});
            return ref;
        }
        rest_.push_back({ref, o.owner, o.side, o.price, left, ++seq_});
        return ref;
    }

    template <class Sink>
    bool cancel(std::uint64_t ref, std::uint32_t owner, Sink&& out) {
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            if (rest_[i].ref != ref || rest_[i].owner != owner) continue;
            out(Event{EventType::Cancelled, Reason::User, rest_[i].side, false, owner, ref,
                      rest_[i].price, rest_[i].qty});
            rest_.erase(rest_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        out(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0, 0});
        return false;
    }

    template <class Sink>
    bool reduce(std::uint64_t ref, std::uint32_t owner, std::uint32_t qty, Sink&& out) {
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            Resting& r = rest_[i];
            if (r.ref != ref || r.owner != owner || qty == 0) continue;
            const std::uint32_t q = qty < r.qty ? qty : r.qty;
            out(Event{EventType::Cancelled, Reason::User, r.side, false, owner, ref, r.price, q});
            r.qty -= q;
            if (r.qty == 0) rest_.erase(rest_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        out(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0, 0});
        return false;
    }

    template <class Sink>
    std::uint64_t replace(std::uint64_t ref, std::uint32_t owner, std::uint32_t price,
                          std::uint32_t qty, bool post_only, Sink&& out) {
        for (const Resting& r : rest_) {
            if (r.ref != ref || r.owner != owner) continue;
            const Side side = r.side;
            cancel(ref, owner, out);
            return submit(NewOrder{owner, side, price, qty, Tif::Day, post_only}, out);
        }
        out(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0, 0});
        return 0;
    }

   private:
    struct Resting {
        std::uint64_t ref;
        std::uint32_t owner;
        Side side;
        std::uint32_t price;
        std::uint32_t qty;
        std::uint64_t seq;
    };

    // Index of the best resting order on a side: best price, then earliest arrival.
    std::optional<std::size_t> best(bool buy_side) const {
        std::optional<std::size_t> b;
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            const Resting& r = rest_[i];
            if ((r.side == Side::Buy) != buy_side) continue;
            if (!b) {
                b = i;
                continue;
            }
            const Resting& c = rest_[*b];
            const bool better = buy_side ? r.price > c.price : r.price < c.price;
            if (better || (r.price == c.price && r.seq < c.seq)) b = i;
        }
        return b;
    }

    Config cfg_;
    std::vector<Resting> rest_;
    std::uint64_t next_ref_ = MatchingEngine<>::kFirstRef;
    std::uint64_t match_ = 0;
    std::uint64_t seq_ = 0;
};

}  // namespace ref
