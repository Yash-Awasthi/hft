#pragma once

// Applies decoded ITCH messages of one stock-locate to a book. A, F add; E, C, X reduce;
// D deletes; U replaces with a new reference and loses priority. Other types do not
// touch the book; P and Q volumes are counted for reconciliation.

#include <cstdint>

#include "book/types.hpp"
#include "feed/itch.hpp"

namespace hft::book {

struct ApplyStats {
    std::uint64_t book_msgs = 0;  // messages that changed the book
    std::uint64_t errors = 0;     // unknown reference, oversized reduce, duplicate add
    std::uint64_t executed = 0;   // shares from E and C
    std::uint64_t hidden = 0;     // shares from P
    std::uint64_t cross = 0;      // shares from Q
};

template <class Book>
struct ItchApply {
    Book& book;
    ApplyStats stats{};
    std::uint64_t seq = 0;  // feed sequence of the message being applied

    void ok(bool r) {
        ++stats.book_msgs;
        stats.errors += !r;
    }
    void operator()(const itch::AddOrder& m) {
        ok(book.add(m.ref, m.side == 'B' ? Side::Buy : Side::Sell, m.shares, m.price, seq));
    }
    void operator()(const itch::OrderExecuted& m) {
        stats.executed += m.shares;
        ok(book.execute(m.ref, m.shares));
    }
    void operator()(const itch::OrderExecutedPrice& m) {
        stats.executed += m.shares;
        ok(book.execute(m.ref, m.shares));
    }
    void operator()(const itch::OrderCancel& m) { ok(book.cancel(m.ref, m.cancelled)); }
    void operator()(const itch::OrderDelete& m) { ok(book.erase(m.ref)); }
    void operator()(const itch::OrderReplace& m) {
        ok(book.replace(m.orig_ref, m.new_ref, m.shares, m.price, seq));
    }
    void operator()(const itch::Trade& m) { stats.hidden += m.shares; }
    void operator()(const itch::CrossTrade& m) { stats.cross += m.shares; }
    template <class T>
    void operator()(const T&) {}
};

}  // namespace hft::book
