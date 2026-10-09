#pragma once

// Price-time matching engine for one symbol, over the L3 book. Handles limit, market and IOC
// orders, post-only orders with a configurable lock/cross policy, self-trade prevention and
// maker/taker fees. Output goes to a sink callable with each Event, so nothing allocates.

#include <algorithm>
#include <cstdint>

#include "book/tick_book.hpp"
#include "book/types.hpp"

namespace hft::engine {

using book::Side;

enum class Tif : std::uint8_t { Day, Ioc };

// Self-trade prevention when an incoming order would trade against its owner's resting one.
enum class Stp : std::uint8_t { None, CancelNewest, CancelOldest, CancelBoth };

// A post-only order that would lock or cross the book: rejected, repriced one tick away from
// the opposite best, or (the 610(e) rescission scenario) allowed to lock but not to cross.
enum class LockPolicy : std::uint8_t { Reject, Reprice, AllowLock };

struct Config {
    std::uint32_t tick = 100;       // price grid, ITCH units ($0.0001)
    std::int64_t maker_rebate = 0;  // micro-dollars per share paid to the maker
    std::int64_t taker_fee = 0;     // micro-dollars per share charged to the taker
    Stp stp = Stp::CancelNewest;
    LockPolicy lock = LockPolicy::Reject;
};

struct NewOrder {
    std::uint32_t owner;  // 0 is reserved for the real market in replay
    Side side;
    std::uint32_t price;  // 0 for a market order
    std::uint32_t qty;
    Tif tif = Tif::Day;
    bool post_only = false;
};

enum class EventType : std::uint8_t { Accepted, Rejected, Fill, Cancelled };
enum class Reason : std::uint8_t {
    None,
    BadPrice,
    BadQty,
    WouldCross,
    WouldLock,
    SelfTrade,
    Ioc,
    User,
    UnknownOrder
};

struct Event {
    EventType type;
    Reason reason = Reason::None;
    Side side;
    bool maker = false;
    std::uint32_t owner;
    std::uint64_t ref;     // exchange order reference
    std::uint32_t price;   // fill price, or the order's price
    std::uint32_t qty;     // filled, accepted or cancelled shares
    std::int64_t fee = 0;  // micro-dollars, negative for a rebate
    std::uint64_t match = 0;
};

template <class Book = book::TickBook>
class MatchingEngine {
   public:
    // Engine references start high so they never collide with ITCH references in replay.
    static constexpr std::uint64_t kFirstRef = 1ull << 39;

    explicit MatchingEngine(Config cfg, std::size_t expected_orders = 1024)
        : cfg_(cfg), book_(expected_orders, cfg.tick) {}

    const Book& book() const { return book_; }
    const Config& config() const { return cfg_; }

    void copy_from(const MatchingEngine& o) {
        cfg_ = o.cfg_;
        book_.copy_from(o.book_);
        next_ref_ = o.next_ref_;
        next_match_ = o.next_match_;
        seq_ = o.seq_;
    }

    // Returns the reference given to the order, or 0 when rejected.
    template <class Sink>
    std::uint64_t submit(NewOrder o, Sink&& sink) {
        if (o.qty == 0) return reject(o, Reason::BadQty, sink);
        if (o.price == 0 ? o.post_only || o.tif != Tif::Ioc
                         : o.price % cfg_.tick != 0 || o.price >= (1u << 31))
            return reject(o, Reason::BadPrice, sink);
        const Side opp = o.side == Side::Buy ? Side::Sell : Side::Buy;

        if (o.post_only) {
            if (const auto f = book_.front(opp); f && !passive(o.side, o.price, f->price)) {
                const bool locks = o.price == f->price;
                if (cfg_.lock == LockPolicy::Reject)
                    return reject(o, locks ? Reason::WouldLock : Reason::WouldCross, sink);
                if (cfg_.lock == LockPolicy::AllowLock) {
                    o.price = f->price;
                } else {
                    if (o.side == Side::Buy && f->price <= cfg_.tick)
                        return reject(o, Reason::BadPrice, sink);
                    o.price = o.side == Side::Buy ? f->price - cfg_.tick : f->price + cfg_.tick;
                }
            }
        }

        const std::uint64_t ref = next_ref_++;
        sink(Event{EventType::Accepted, Reason::None, o.side, false, o.owner, ref, o.price, o.qty});

        std::uint32_t left = o.qty;
        while (left && !o.post_only) {
            const auto f = book_.front(opp);
            if (!f || (o.price != 0 && passive(o.side, o.price, f->price))) break;
            if (cfg_.stp != Stp::None && f->owner == o.owner && o.owner != 0) {
                if (cfg_.stp != Stp::CancelNewest) {
                    book_.erase(f->ref);
                    sink(Event{EventType::Cancelled, Reason::SelfTrade, f->side, false, f->owner,
                               f->ref, f->price, f->qty});
                }
                if (cfg_.stp == Stp::CancelOldest) continue;
                sink(Event{EventType::Cancelled, Reason::SelfTrade, o.side, false, o.owner, ref,
                           o.price, left});
                return ref;
            }
            const std::uint32_t q = std::min(left, f->qty);
            book_.execute(f->ref, q);
            const std::uint64_t m = ++next_match_;
            sink(Event{EventType::Fill, Reason::None, f->side, true, f->owner, f->ref, f->price, q,
                       -cfg_.maker_rebate * q, m});
            sink(Event{EventType::Fill, Reason::None, o.side, false, o.owner, ref, f->price, q,
                       cfg_.taker_fee * q, m});
            left -= q;
        }
        if (!left) return ref;
        if (o.tif == Tif::Ioc) {
            sink(Event{EventType::Cancelled, Reason::Ioc, o.side, false, o.owner, ref, o.price,
                       left});
            return ref;
        }
        book_.add(ref, o.side, left, o.price, ++seq_, o.owner);
        return ref;
    }

    // Cancels all remaining shares of an order its owner placed.
    template <class Sink>
    bool cancel(std::uint64_t ref, std::uint32_t owner, Sink&& sink) {
        const auto v = book_.order(ref);
        if (!v || v->owner != owner) {
            sink(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0,
                       0});
            return false;
        }
        book_.erase(ref);
        sink(Event{EventType::Cancelled, Reason::User, v->side, false, owner, ref, v->price,
                   v->qty});
        return true;
    }

    // Cancels `qty` shares and keeps the order's queue position, like an ITCH X message.
    template <class Sink>
    bool reduce(std::uint64_t ref, std::uint32_t owner, std::uint32_t qty, Sink&& sink) {
        const auto v = book_.order(ref);
        if (!v || v->owner != owner || qty == 0) {
            sink(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0,
                       0});
            return false;
        }
        qty = std::min(qty, v->qty);
        book_.cancel(ref, qty);
        sink(Event{EventType::Cancelled, Reason::User, v->side, false, owner, ref, v->price, qty});
        return true;
    }

    // Cancel and new order on the same side; the new order loses queue priority and may trade.
    template <class Sink>
    std::uint64_t replace(std::uint64_t ref, std::uint32_t owner, std::uint32_t price,
                          std::uint32_t qty, bool post_only, Sink&& sink) {
        const auto v = book_.order(ref);
        if (!v || v->owner != owner) {
            sink(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0,
                       0});
            return 0;
        }
        cancel(ref, owner, sink);
        return submit(NewOrder{owner, v->side, price, qty, Tif::Day, post_only}, sink);
    }

   private:
    // True when a resting order at `book_px` on the other side does not trade with `px`.
    static bool passive(Side side, std::uint32_t px, std::uint32_t book_px) {
        return side == Side::Buy ? px < book_px : px > book_px;
    }

    template <class Sink>
    std::uint64_t reject(const NewOrder& o, Reason r, Sink& sink) {
        sink(Event{EventType::Rejected, r, o.side, false, o.owner, 0, o.price, o.qty});
        return 0;
    }

    Config cfg_;
    Book book_;
    std::uint64_t next_ref_ = kFirstRef;
    std::uint64_t next_match_ = 0;
    std::uint64_t seq_ = 0;
};

}  // namespace hft::engine
