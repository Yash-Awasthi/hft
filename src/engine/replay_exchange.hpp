#pragma once

// Exchange side of a replay backtest for one symbol. Real ITCH orders (owner 0) and our
// virtual orders share the book's queues. A real execution of order R fills, first, every
// virtual order the aggressor would have reached before R: a better price, or the same price
// earlier in the queue. Those shares are counted as diverted, since in the data R got them.
// Virtual orders that take liquidity consume real resting shares, tracked per order so the
// same shares are not taken twice. DESIGN.md section 2.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "book/id_map.hpp"
#include "book/tick_book.hpp"
#include "engine/matching.hpp"
#include "feed/itch.hpp"

namespace hft::engine {

// Queue: fill by queue priority, including displayed priority over hidden executions at the
// same price. TradeThrough: fill only when a trade prints strictly beyond our price, the
// conservative bound of section 2.
enum class FillRule : std::uint8_t { Queue, TradeThrough };

struct Divergence {
    std::uint64_t diverted = 0;       // real executed shares our orders would have taken
    std::uint64_t taken = 0;          // real resting shares our marketable orders consumed
    std::uint64_t through_fills = 0;  // virtual fills from trades beyond our price
    std::uint64_t hidden_fills = 0;   // virtual fills from executions of hidden orders
};

template <class Book = book::TickBook<>>
class ReplayExchange {
   public:
    ReplayExchange(Config cfg, FillRule rule, std::size_t expected_orders = 1024)
        : cfg_(cfg), rule_(rule), book_(expected_orders, cfg.tick), consumed_(64) {
        virt_.reserve(64);
    }

    const Book& book() const { return book_; }

    // Best prices and sizes of real orders only. Levels holding only virtual orders are
    // skipped; few virtual orders are live, so the walk is short.
    book::Bbo real_bbo() const {
        book::Bbo b;
        for (const Side s : {Side::Buy, Side::Sell}) {
            for (auto f = book_.front(s); f; f = book_.next_level(s, f->price)) {
                std::uint64_t q = book_.level_qty(s, f->price);
                for (const Virt& v : virt_)
                    if (v.side == s && v.price == f->price) q -= book_.order(v.ref)->qty;
                if (!q) continue;
                (s == Side::Buy ? b.bid_px : b.ask_px) = f->price;
                (s == Side::Buy ? b.bid_qty : b.ask_qty) = q;
                break;
            }
        }
        return b;
    }
    const Divergence& divergence() const { return div_; }

    // Applies one ITCH message of this symbol; virtual fills go to `sink`.
    template <class Sink>
    void on_itch(const std::uint8_t* msg, std::size_t len, Sink&& sink) {
        Apply<Sink> a{*this, sink};
        itch::dispatch(msg, len, a);
    }

    // A virtual order arriving at the exchange: takes real liquidity if marketable, then rests
    // at the back of its level unless IOC. Returns its reference, or 0 when rejected.
    template <class Sink>
    std::uint64_t submit(NewOrder o, Sink&& sink) {
        if (o.owner == 0 || o.qty == 0 || o.price == 0 || o.price % cfg_.tick ||
            o.price >= (1u << 31)) {
            sink(Event{EventType::Rejected, Reason::BadPrice, o.side, false, o.owner, 0, o.price,
                       o.qty});
            return 0;
        }
        const Side opp = o.side == Side::Buy ? Side::Sell : Side::Buy;
        if (o.post_only) {
            const auto f = book_.front(opp);
            if (f && !passive(o.side, o.price, f->price)) {
                sink(Event{EventType::Rejected,
                           f->price == o.price ? Reason::WouldLock : Reason::WouldCross, o.side,
                           false, o.owner, 0, o.price, o.qty});
                return 0;
            }
        }
        const std::uint64_t ref = next_ref_++;
        sink(Event{EventType::Accepted, Reason::None, o.side, false, o.owner, ref, o.price, o.qty});
        std::uint32_t left = take(o, ref, sink);
        if (!left) return ref;
        if (o.tif == Tif::Ioc) {
            sink(Event{EventType::Cancelled, Reason::Ioc, o.side, false, o.owner, ref, o.price,
                       left});
            return ref;
        }
        rest(ref, o.side, left, o.price, o.owner, (max_ref_ << 20) | ++virtual_count_);
        return ref;
    }

    // Shares queued ahead of a resting virtual order at its level, kept in O(1) per event:
    // it falls whenever an order that arrived earlier at that level loses shares.
    std::uint64_t queue_ahead(std::uint64_t ref) const {
        for (const Virt& v : virt_)
            if (v.ref == ref) return v.ahead;
        return 0;
    }

    // Takes the real order `real_ref` out of the replayed book from its arrival on and keeps
    // it as a virtual order of `owner` at the same queue position. Its executions in the data
    // become executions of the virtual order at that position; its cancels and deletes apply
    // to it. This is the queue-tracking check of DESIGN.md section 2.
    void mirror(std::uint64_t real_ref, std::uint32_t owner) { mirrors_[real_ref] = {owner, 0}; }
    std::uint64_t mirrored(std::uint64_t real_ref) const {
        const auto it = mirrors_.find(real_ref);
        return it == mirrors_.end() ? 0 : it->second.vref;
    }

    template <class Sink>
    bool cancel(std::uint64_t ref, std::uint32_t owner, Sink&& sink) {
        const auto v = book_.order(ref);
        if (!v || v->owner != owner || owner == 0) {
            sink(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0,
                       0});
            return false;
        }
        drop_virtual(*v, v->qty);
        sink(Event{EventType::Cancelled, Reason::User, v->side, false, owner, ref, v->price,
                   v->qty});
        return true;
    }

   private:
    template <class Sink>
    struct Apply {
        ReplayExchange& x;
        Sink& sink;
        void operator()(const itch::AddOrder& m) {
            const Side side = m.side == 'B' ? Side::Buy : Side::Sell;
            if (auto it = x.mirrors_.find(m.ref); it != x.mirrors_.end()) {
                it->second.vref = x.next_ref_++;
                x.rest(it->second.vref, side, m.shares, m.price, it->second.owner,
                       x.real_key(m.ref));
                return;
            }
            x.book_.add(m.ref, side, m.shares, m.price, x.real_key(m.ref));
        }
        void operator()(const itch::OrderExecuted& m) { x.real_exec(m.ref, m.shares, sink); }
        void operator()(const itch::OrderExecutedPrice& m) { x.real_exec(m.ref, m.shares, sink); }
        void operator()(const itch::OrderCancel& m) { x.real_reduce(m.ref, m.cancelled); }
        void operator()(const itch::OrderDelete& m) { x.real_reduce(m.ref, ~0u); }
        void operator()(const itch::OrderReplace& m) {
            const std::uint64_t ref = x.resolve(m.orig_ref);
            const auto v = x.book_.order(ref);
            if (!v) return;
            x.real_reduce(m.orig_ref, ~0u);
            x.mirrors_.erase(m.orig_ref);
            x.book_.add(m.new_ref, v->side, m.shares, m.price, x.real_key(m.new_ref));
        }
        // ITCH sets the side of every P message to 'B', so a hidden print is matched against
        // both sides: it reaches bids at or above its price and asks at or below it. A
        // displayed order outranks the hidden one at the same price.
        void operator()(const itch::Trade& m) {
            const std::uint32_t got =
                x.virtual_fills(Side::Buy, m.price, ~0ull, m.shares, true, sink);
            x.virtual_fills(Side::Sell, m.price, ~0ull, m.shares - got, true, sink);
        }
        template <class T>
        void operator()(const T&) {}
    };

    static bool passive(Side side, std::uint32_t px, std::uint32_t book_px) {
        return side == Side::Buy ? px < book_px : px > book_px;
    }
    static bool better(Side side, std::uint32_t a, std::uint32_t b) {
        return side == Side::Buy ? a > b : a < b;
    }

    struct Virt {
        std::uint64_t ref, seq, ahead;
        std::uint32_t price;
        Side side;
    };
    struct Mirror {
        std::uint32_t owner;
        std::uint64_t vref;
    };

    // Book reference standing for a real reference: the virtual copy of a mirrored order.
    std::uint64_t resolve(std::uint64_t real_ref) const {
        const auto it = mirrors_.find(real_ref);
        return it == mirrors_.end() ? real_ref : it->second.vref;
    }

    // Queue priority follows the order reference, which Nasdaq assigns on receipt: orders
    // received before the open are displayed at 09:30 yet keep their earlier rank. A virtual
    // order ranks after every reference seen so far.
    std::uint64_t real_key(std::uint64_t ref) {
        max_ref_ = std::max(max_ref_, ref);
        return ref << 20;
    }

    void rest(std::uint64_t ref, Side side, std::uint32_t qty, std::uint32_t price,
              std::uint32_t owner, std::uint64_t key) {
        std::uint64_t ahead = 0;
        auto f = book_.front(side);
        while (f && f->price != price) f = book_.next_level(side, f->price);
        for (; f; f = book_.behind(f->ref))
            if (f->seq < key) ahead += f->qty;
        book_.add(ref, side, qty, price, key, owner);
        virt_.push_back({ref, key, ahead, price, side});
    }

    // `q` shares left the queue at (side, price) from an order that arrived at `seq`.
    void removed(Side side, std::uint32_t price, std::uint64_t seq, std::uint64_t q) {
        for (Virt& v : virt_)
            if (v.side == side && v.price == price && v.seq > seq) v.ahead -= std::min(v.ahead, q);
    }

    void drop_virtual(const book::OrderView& v, std::uint32_t q) {
        if (q >= v.qty) {
            book_.erase(v.ref);
            std::erase_if(virt_, [&](const Virt& w) { return w.ref == v.ref; });
        } else {
            book_.cancel(v.ref, q);
        }
        removed(v.side, v.price, v.seq, std::min(q, v.qty));
    }

    // Partial cancel (or delete with ~0) of a real order, or of the virtual copy if mirrored.
    void real_reduce(std::uint64_t real_ref, std::uint32_t q) {
        const std::uint64_t ref = resolve(real_ref);
        const auto r = book_.order(ref);
        if (!r) return;
        if (ref != real_ref) {
            drop_virtual(*r, q);
            return;
        }
        const std::uint32_t n = std::min(q, r->qty);
        book_.cancel(ref, n);
        removed(r->side, r->price, r->seq, n);
        forget_if_gone(ref);
    }

    template <class Sink>
    void real_exec(std::uint64_t real_ref, std::uint32_t shares, Sink& sink) {
        const std::uint64_t ref = resolve(real_ref);
        const auto r = book_.order(ref);
        if (!r) return;
        if (ref != real_ref) {
            // The aggressor reached the mirrored order's position: fill it and any virtual
            // order ranking ahead of it.
            virtual_fills(r->side, r->price, r->seq + 1, shares, false, sink, true);
            return;
        }
        const std::uint32_t got = virtual_fills(r->side, r->price, r->seq, shares, false, sink);
        div_.diverted += got;
        book_.execute(ref, shares);
        removed(r->side, r->price, r->seq, shares);
        forget_if_gone(ref);
    }

    // Fills virtual orders on `side` that rank ahead of a trade of `shares` at `px` against an
    // order that arrived at `arrival` (all arrivals for hidden prints, which rank last).
    template <class Sink>
    // own: the data executed a mirrored order, so queue priority applies under either rule.
    std::uint32_t virtual_fills(Side side, std::uint32_t px, std::uint64_t arrival,
                                std::uint32_t shares, bool hidden, Sink& sink, bool own = false) {
        std::uint32_t total = 0;
        while (shares) {
            // Best-ranked eligible virtual order; few are live at once, so a scan suffices.
            std::optional<book::OrderView> best;
            for (const Virt& w : virt_) {
                const auto v = book_.order(w.ref);
                if (v->side != side) continue;
                const bool through = better(side, v->price, px);
                const bool ahead = v->price == px && v->seq < arrival;
                if (!(rule_ == FillRule::TradeThrough && !own ? through : through || ahead))
                    continue;
                if (!best || better(side, v->price, best->price) ||
                    (v->price == best->price && v->seq < best->seq))
                    best = v;
            }
            if (!best) break;
            const std::uint32_t q = std::min(shares, best->qty);
            drop_virtual(*best, q);
            div_.through_fills += better(side, best->price, px);
            div_.hidden_fills += hidden;
            sink(Event{EventType::Fill, Reason::None, side, true, best->owner, best->ref,
                       best->price, q, -cfg_.maker_rebate * q, ++next_match_});
            shares -= q;
            total += q;
        }
        return total;
    }

    // Marketable virtual order against the book: real orders give only the shares no earlier
    // virtual order consumed. Resting virtual orders are skipped: one strategy per replay.
    template <class Sink>
    std::uint32_t take(const NewOrder& o, std::uint64_t ref, Sink& sink) {
        const Side opp = o.side == Side::Buy ? Side::Sell : Side::Buy;
        std::uint32_t left = o.qty;
        for (auto f = book_.front(opp); left && f && !passive(o.side, o.price, f->price);
             f = book_.behind(f->ref).has_value() ? book_.behind(f->ref) : next_level(opp, *f)) {
            if (f->owner != 0) continue;  // our own resting orders never trade with us
            const std::uint32_t used = consumed_.find(f->ref);
            const std::uint32_t avail = f->qty - (used == book::kNoOrder ? 0 : used);
            if (!avail) continue;
            const std::uint32_t q = std::min(left, avail);
            if (used != book::kNoOrder) consumed_.erase(f->ref);
            consumed_.insert(f->ref, (used == book::kNoOrder ? 0 : used) + q);
            div_.taken += q;
            sink(Event{EventType::Fill, Reason::None, o.side, false, o.owner, ref, f->price, q,
                       cfg_.taker_fee * q, ++next_match_});
            left -= q;
        }
        return left;
    }

    // First order of the next worse level on `side` after the level of `f`.
    std::optional<book::OrderView> next_level(Side side, const book::OrderView& f) const {
        return book_.next_level(side, f.price);
    }

    void forget_if_gone(std::uint64_t ref) {
        if (!book_.order(ref)) consumed_.erase(ref);
    }

    Config cfg_;
    FillRule rule_;
    Book book_;
    book::LinearMap consumed_;  // real reference -> shares taken by our orders
    std::vector<Virt> virt_;
    std::unordered_map<std::uint64_t, Mirror> mirrors_;  // experiment only, off the hot path
    Divergence div_;
    std::uint64_t max_ref_ = 0, virtual_count_ = 0;
    std::uint64_t next_ref_ = MatchingEngine<>::kFirstRef;
    std::uint64_t next_match_ = 0;
};

}  // namespace hft::engine
