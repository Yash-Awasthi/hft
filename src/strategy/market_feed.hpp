#pragma once

// Applies ITCH messages of one symbol to its book and describes each as a MarketEvent,
// reading the book before the update to know the side, price and queue rank of the order.

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "feed/itch.hpp"
#include "strategy/event.hpp"

namespace hft::strategy {

template <class Book = book::TickBook>
class MarketFeed {
   public:
    explicit MarketFeed(Book& b) : book_(b) {}

    // Returns false for messages that neither change the book nor carry a trade.
    bool on_itch(const std::uint8_t* msg, std::size_t len, std::uint64_t seq, MarketEvent& out) {
        out = MarketEvent{};
        out.seq = seq;
        Describe d{*this, out};
        itch::dispatch(msg, len, d);
        if (out.kind == EventKind::Other) return false;
        book::ItchApply<Book> ap{book_};
        ap.seq = seq;
        itch::dispatch(msg, len, ap);
        return true;
    }

   private:
    struct Describe {
        MarketFeed& f;
        MarketEvent& e;
        void resting(std::uint64_t ref, EventKind k, std::uint32_t shares, const itch::Header& h) {
            e.ts = h.timestamp;
            const auto o = f.book_.order(ref);
            if (!o) return;  // unknown reference: leave as Other so the book is untouched
            e.kind = k;
            e.side = o->side;
            e.price = o->price;
            e.shares = shares ? shares : o->qty;
            const auto best = f.book_.best_level(o->side);
            e.at_best = best && best->price == o->price;
        }
        void operator()(const itch::AddOrder& m) {
            e.kind = EventKind::Add;
            e.ts = m.h.timestamp;
            e.side = m.side == 'B' ? book::Side::Buy : book::Side::Sell;
            e.price = m.price;
            e.shares = m.shares;
            const auto best = f.book_.best_level(e.side);
            e.at_best = !best || best->price == m.price ||
                        (e.side == book::Side::Buy ? m.price > best->price : m.price < best->price);
        }
        void operator()(const itch::OrderExecuted& m) { executed(m.ref, m.shares, m.h); }
        void operator()(const itch::OrderExecutedPrice& m) { executed(m.ref, m.shares, m.h); }
        // The aggressor is opposite the resting order: a resting bid hit is a sell.
        void executed(std::uint64_t ref, std::uint32_t shares, const itch::Header& h) {
            resting(ref, EventKind::Execute, shares, h);
            if (e.kind == EventKind::Execute) e.aggressor = e.side == book::Side::Buy ? -1 : 1;
        }
        void operator()(const itch::OrderCancel& m) {
            resting(m.ref, EventKind::Cancel, m.cancelled, m.h);
        }
        void operator()(const itch::OrderDelete& m) { resting(m.ref, EventKind::Delete, 0, m.h); }
        void operator()(const itch::OrderReplace& m) {
            resting(m.orig_ref, EventKind::Replace, 0, m.h);
        }
        // ITCH has set the side of every P message to 'B' since 2014, so the aggressor is
        // inferred from the print against the mid (Lee and Ready), unknown at the mid.
        void operator()(const itch::Trade& m) {
            e.kind = EventKind::Hidden;
            e.ts = m.h.timestamp;
            e.price = m.price;
            e.shares = m.shares;
            const book::Bbo q = f.book_.bbo();
            if (q.bid_px && q.ask_px) {
                const std::uint64_t twice = 2ull * m.price,
                                    mid2 = std::uint64_t{q.bid_px} + q.ask_px;
                e.aggressor = twice > mid2 ? 1 : twice < mid2 ? -1 : 0;
            }
            e.side = e.aggressor > 0 ? book::Side::Sell : book::Side::Buy;
        }
        void operator()(const itch::CrossTrade& m) {
            e.kind = EventKind::Cross;
            e.ts = m.h.timestamp;
            e.price = m.price;
            e.shares = static_cast<std::uint32_t>(m.shares);
        }
        void operator()(const itch::Imbalance& m) {
            e.kind = EventKind::Imbalance;
            e.ts = m.h.timestamp;
            e.paired = m.paired;
            const auto imb = static_cast<std::int64_t>(m.imbalance);
            e.imbalance = m.direction == 'B' ? imb : m.direction == 'S' ? -imb : 0;
        }
        template <class T>
        void operator()(const T&) {}
    };

    Book& book_;
};

}  // namespace hft::strategy
