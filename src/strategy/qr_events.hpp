#pragma once

// Event record for calibrating the queue-reactive model (DESIGN.md section 5, method C) from
// one symbol's ITCH flow, real or simulated by sources::QueueReactive.
//
// Reference price p_ref: half a tick off the price grid, inside the quotes; it stays put while
// it remains inside them and otherwise moves to the nearest admissible value. Each limit
// insertion (L), cancellation (C) or execution (M) at a window level i = 1..K of either side
// is recorded with the shares at all 2K levels just before it. A replace is a cancellation
// and an insertion. Depletion episodes of level 1 end either with p_ref moving towards the
// emptied side (a move) or with level 1 refilled first; their counts estimate theta. Every
// p_ref change also gets a record of kind 3 holding the shares before it, so the records cut
// time into intervals of constant state.

#include <cstdint>
#include <string>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace hft::strategy {

struct QrRecord {
    std::vector<std::uint64_t> ts;
    std::vector<std::int8_t> kind, side, level;  // kind 0 L, 1 C, 2 M, 3 move; side 0 bid, 1 ask
    std::vector<std::uint32_t> shares;
    std::vector<std::uint8_t> after_move;  // same timestamp as the last p_ref change
    std::vector<std::uint64_t> q;          // 2K per event: bid levels 1..K, ask levels 1..K
    std::vector<std::uint64_t> moves_ts;   // p_ref changes
    std::vector<std::int8_t> moves_dir;
    std::uint64_t episodes_moved = 0, episodes_refilled = 0;
};

class QrRecorder {
   public:
    QrRecorder(int K, std::uint32_t tick, std::uint64_t start_ns, std::uint64_t end_ns)
        : K_(K), tick_(tick), start_(start_ns), end_(end_ns) {}

    void run(const std::string& store, std::uint16_t locate) {
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        while (rd.next(rec)) on_itch(rec.data, rec.len);
    }

    void on_itch(const std::uint8_t* msg, std::size_t len) {
        ts_ = itch::detail::read_header(msg).timestamp;
        pending_.clear();
        Pre pre{*this};
        itch::dispatch(msg, len, pre);
        const bool window = ts_ >= start_ && ts_ < end_ && pref_;
        if (window) {
            snapshot();
            for (const Ev& e : pending_) record(e);
        }
        itch::dispatch(msg, len, apply_);
        if (update_pref(window)) push(3, 0, 0, 0);
    }

    const QrRecord& record() const { return r_; }

   private:
    struct Ev {
        std::int8_t kind;
        book::Side side;
        std::uint32_t px, shares;
    };

    // Collects the events of a message against the book before it is applied.
    struct Pre {
        QrRecorder& r;
        void order(std::uint64_t ref, std::int8_t kind, std::uint32_t shares) {
            if (const auto o = r.b_.order(ref)) r.pending_.push_back({kind, o->side, o->price, shares});
        }
        void operator()(const itch::AddOrder& m) {
            r.pending_.push_back({0, m.side == 'B' ? book::Side::Buy : book::Side::Sell, m.price, m.shares});
        }
        void operator()(const itch::OrderExecuted& m) { order(m.ref, 2, m.shares); }
        void operator()(const itch::OrderExecutedPrice& m) { order(m.ref, 2, m.shares); }
        void operator()(const itch::OrderCancel& m) { order(m.ref, 1, m.cancelled); }
        void operator()(const itch::OrderDelete& m) {
            if (const auto o = r.b_.order(m.ref)) r.pending_.push_back({1, o->side, o->price, o->qty});
        }
        void operator()(const itch::OrderReplace& m) {
            if (const auto o = r.b_.order(m.orig_ref)) {
                r.pending_.push_back({1, o->side, o->price, o->qty});
                r.pending_.push_back({0, o->side, m.price, m.shares});
            }
        }
        template <class T>
        void operator()(const T&) {}
    };

    // Level 1..K of a price on its side relative to p_ref, 0 outside the window.
    int level(book::Side s, std::uint32_t px) const {
        const std::int64_t d = s == book::Side::Buy ? std::int64_t{pref_} - px : std::int64_t{px} - pref_;
        const std::int64_t half = tick_ / 2;
        if (d < half || (d - half) % tick_) return 0;
        const auto i = static_cast<int>((d - half) / tick_) + 1;
        return i <= K_ ? i : 0;
    }
    std::uint32_t level_px(int s, int i) const {
        const std::uint32_t off = static_cast<std::uint32_t>(i - 1) * tick_ + tick_ / 2;
        return s == 0 ? pref_ - off : pref_ + off;
    }
    std::uint64_t qty(int s, int i) const {
        return b_.level_qty(s == 0 ? book::Side::Buy : book::Side::Sell, level_px(s, i));
    }

    void snapshot() {
        snap_.clear();
        for (int s = 0; s < 2; ++s)
            for (int k = 1; k <= K_; ++k) snap_.push_back(qty(s, k));
    }

    void push(std::int8_t kind, int side, int lvl, std::uint32_t shares) {
        r_.ts.push_back(ts_);
        r_.kind.push_back(kind);
        r_.side.push_back(static_cast<std::int8_t>(side));
        r_.level.push_back(static_cast<std::int8_t>(lvl));
        r_.shares.push_back(shares);
        r_.after_move.push_back(!r_.moves_ts.empty() && r_.moves_ts.back() == ts_);
        r_.q.insert(r_.q.end(), snap_.begin(), snap_.end());
    }

    void record(const Ev& e) {
        if (const int i = level(e.side, e.px))
            push(e.kind, e.side == book::Side::Buy ? 0 : 1, i, e.shares);
    }

    // Returns whether p_ref changed inside the window.
    bool update_pref(bool window) {
        const book::Bbo q = b_.bbo();
        // p_ref holds while a side is empty or quotes are off the grid (sub-penny).
        const bool valid = q.bid_px && q.ask_px && q.ask_px > q.bid_px && !(q.bid_px % tick_) &&
                           !(q.ask_px % tick_);
        if (!valid && !pref_) return false;
        const std::uint32_t old = pref_;
        if (valid) {
            const std::uint32_t lo = q.bid_px + tick_ / 2, hi = q.ask_px - tick_ / 2;
            if (!pref_ || pref_ < lo || pref_ > hi) pref_ = pref_ && pref_ > hi ? hi : lo;
        }
        const int dir = !old || pref_ == old ? 0 : (pref_ > old ? 1 : -1);
        if (dir && window && !snap_.empty())
            r_.moves_ts.push_back(ts_), r_.moves_dir.push_back(static_cast<std::int8_t>(dir));
        for (int s = 0; s < 2; ++s) {
            if (pending_episode_[s] && window) {
                if (dir == (s == 0 ? -1 : 1)) ++r_.episodes_moved, pending_episode_[s] = false;
                else if (dir) pending_episode_[s] = false;  // moved away: neither outcome
                else if (qty(s, 1)) ++r_.episodes_refilled, pending_episode_[s] = false;
            }
            const bool empty = qty(s, 1) == 0;
            if (empty && !was_empty_[s] && !dir && window) pending_episode_[s] = true;
            was_empty_[s] = empty;
        }
        return dir && window;
    }

    int K_;
    std::uint32_t tick_;
    std::uint64_t start_, end_, ts_ = 0;
    std::uint32_t pref_ = 0;
    book::TickBook<> b_;
    book::ItchApply<book::TickBook<>> apply_{b_};
    std::vector<Ev> pending_;
    std::vector<std::uint64_t> snap_;
    bool pending_episode_[2] = {false, false}, was_empty_[2] = {false, false};
    QrRecord r_;
};

}  // namespace hft::strategy
