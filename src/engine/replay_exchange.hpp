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
        book_.add(ref, o.side, left, o.price, ++arrival_, o.owner);
        virt_.push_back(ref);
        return ref;
    }

    template <class Sink>
    bool cancel(std::uint64_t ref, std::uint32_t owner, Sink&& sink) {
        const auto v = book_.order(ref);
        if (!v || v->owner != owner || owner == 0) {
            sink(Event{EventType::Rejected, Reason::UnknownOrder, Side::Buy, false, owner, ref, 0,
                       0});
            return false;
        }
        book_.erase(ref);
        std::erase(virt_, ref);
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
            x.book_.add(m.ref, m.side == 'B' ? Side::Buy : Side::Sell, m.shares, m.price,
                        ++x.arrival_);
        }
        void operator()(const itch::OrderExecuted& m) { x.real_exec(m.ref, m.shares, sink); }
        void operator()(const itch::OrderExecutedPrice& m) { x.real_exec(m.ref, m.shares, sink); }
        void operator()(const itch::OrderCancel& m) {
            x.book_.cancel(m.ref, m.cancelled);
            x.forget_if_gone(m.ref);
        }
        void operator()(const itch::OrderDelete& m) {
            x.book_.erase(m.ref);
            x.forget_if_gone(m.ref);
        }
        void operator()(const itch::OrderReplace& m) {
            const auto v = x.book_.order(m.orig_ref);
            if (!v) return;
            x.book_.erase(m.orig_ref);
            x.forget_if_gone(m.orig_ref);
            x.book_.add(m.new_ref, v->side, m.shares, m.price, ++x.arrival_);
        }
        // The side is that of the hidden resting order.
        void operator()(const itch::Trade& m) {
            x.virtual_fills(m.side == 'B' ? Side::Buy : Side::Sell, m.price, ~0ull, m.shares, true,
                            sink);
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

    template <class Sink>
    void real_exec(std::uint64_t ref, std::uint32_t shares, Sink& sink) {
        const auto r = book_.order(ref);
        if (!r) return;
        const std::uint32_t got = virtual_fills(r->side, r->price, r->seq, shares, false, sink);
        div_.diverted += got;
        book_.execute(ref, shares);
        forget_if_gone(ref);
    }

    // Fills virtual orders on `side` that rank ahead of a trade of `shares` at `px` against an
    // order that arrived at `arrival` (all arrivals for hidden prints, which rank last).
    template <class Sink>
    std::uint32_t virtual_fills(Side side, std::uint32_t px, std::uint64_t arrival,
                                std::uint32_t shares, bool hidden, Sink& sink) {
        std::uint32_t total = 0;
        while (shares) {
            // Best-ranked eligible virtual order; few are live at once, so a scan suffices.
            std::optional<book::OrderView> best;
            for (std::uint64_t ref : virt_) {
                const auto v = book_.order(ref);
                if (v->side != side) continue;
                const bool through = better(side, v->price, px);
                const bool ahead = v->price == px && v->seq < arrival;
                if (!(rule_ == FillRule::TradeThrough ? through : through || ahead)) continue;
                if (!best || better(side, v->price, best->price) ||
                    (v->price == best->price && v->seq < best->seq))
                    best = v;
            }
            if (!best) break;
            const std::uint32_t q = std::min(shares, best->qty);
            book_.execute(best->ref, q);
            if (q == best->qty) std::erase(virt_, best->ref);
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
    std::vector<std::uint64_t> virt_;
    Divergence div_;
    std::uint64_t arrival_ = 0;
    std::uint64_t next_ref_ = MatchingEngine<>::kFirstRef;
    std::uint64_t next_match_ = 0;
};

}  // namespace hft::engine
