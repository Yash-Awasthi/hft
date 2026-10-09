#pragma once

// Signed trades of one symbol for the impact study (DESIGN.md section 6).
//
// Every execution (E, C against displayed orders; P against hidden ones) is signed by the
// aggressor: a resting buy executed means a seller-initiated trade (-1). Nasdaq fills the side
// of every P message with 'B', so hidden executions are signed by price against the mid and
// dropped at the mid. Executions of one sign
// within one timestamp are one trade (a market order sweeping several orders or levels), with
// the mid just before the first of them and just after the last. Executions are also kept one
// by one with the resting order's market participant ID when the order was attributed ('F'),
// since attributed passive fills from one ID are a metaorder proxy.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace hft::strategy {

struct TradeRecord {
    std::vector<std::uint64_t> ts;
    std::vector<std::int8_t> sign;
    std::vector<std::uint32_t> shares;
    std::vector<double> notional, mid_before, mid_after;  // dollars
    // One row per execution.
    std::vector<std::uint64_t> exec_ts;
    std::vector<std::int8_t> exec_sign;
    std::vector<std::uint32_t> exec_shares, exec_mpid;  // mpid: four ASCII bytes, 0 if anonymous
    std::vector<std::uint8_t> exec_hidden;
};

class TradeRecorder {
   public:
    TradeRecorder(std::uint64_t start_ns, std::uint64_t end_ns) : start_(start_ns), end_(end_ns) {}

    void run(const std::string& store, std::uint16_t locate) {
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        while (rd.next(rec)) on_itch(rec.data, rec.len);
        close();
    }

    void on_itch(const std::uint8_t* msg, std::size_t len) {
        ts_ = itch::detail::read_header(msg).timestamp;
        const double before = mid();
        Pre pre{*this, before};
        itch::dispatch(msg, len, pre);
        itch::dispatch(msg, len, apply_);
        // An open trade ends at the first message that is not one of its executions.
        if (!pre.executed && open_) close();
        if (open_) r_.mid_after.back() = mid();
    }

    // Ends the last open trade; call after the stream.
    void close() { open_ = false; }

    const TradeRecord& record() const { return r_; }

   private:
    struct Pre {
        TradeRecorder& t;
        double before;
        bool executed = false;
        void exec(std::int8_t sign, std::uint32_t shares, std::uint32_t price, std::uint32_t mpid,
                  bool hidden) {
            if (!sign) return;
            executed = true;
            if (t.ts_ < t.start_ || t.ts_ >= t.end_) return;
            t.execution(sign, shares, price, mpid, hidden, before);
        }
        void operator()(const itch::AddOrder& m) {
            if (m.mpid[0] && m.mpid[0] != ' ') {
                std::uint32_t id;
                std::memcpy(&id, m.mpid, 4);
                t.mpid_[m.ref] = id;
            }
        }
        void operator()(const itch::OrderReplace& m) {
            if (const auto it = t.mpid_.find(m.orig_ref); it != t.mpid_.end()) {
                t.mpid_[m.new_ref] = it->second;
                t.mpid_.erase(it);
            }
        }
        void operator()(const itch::OrderDelete& m) { t.mpid_.erase(m.ref); }
        void operator()(const itch::OrderExecuted& m) {
            if (const auto o = t.b_.order(m.ref)) exec(sign(o->side), m.shares, o->price, id(m.ref), false);
        }
        void operator()(const itch::OrderExecutedPrice& m) {
            if (const auto o = t.b_.order(m.ref)) exec(sign(o->side), m.shares, m.price, id(m.ref), false);
        }
        void operator()(const itch::Trade& m) {
            const double px = m.price / 1e4;
            const std::int8_t s = !std::isfinite(before) || px == before ? 0 : (px > before ? 1 : -1);
            exec(s, m.shares, m.price, 0, true);
        }
        static std::int8_t sign(book::Side resting) { return resting == book::Side::Buy ? -1 : 1; }
        template <class T>
        void operator()(const T&) {}
        std::uint32_t id(std::uint64_t ref) const {
            const auto it = t.mpid_.find(ref);
            return it == t.mpid_.end() ? 0 : it->second;
        }
    };

    double mid() const {
        const book::Bbo q = b_.bbo();
        return q.bid_px && q.ask_px && q.ask_px > q.bid_px ? (q.bid_px + q.ask_px) / 2e4 : NAN;
    }

    void execution(std::int8_t sign, std::uint32_t shares, std::uint32_t price, std::uint32_t mpid,
                   bool hidden, double before) {
        r_.exec_ts.push_back(ts_);
        r_.exec_sign.push_back(sign);
        r_.exec_shares.push_back(shares);
        r_.exec_mpid.push_back(mpid);
        r_.exec_hidden.push_back(hidden);
        if (open_ && r_.ts.back() == ts_ && r_.sign.back() == sign) {
            r_.shares.back() += shares;
            r_.notional.back() += shares * (price / 1e4);
            return;
        }
        open_ = true;
        r_.ts.push_back(ts_);
        r_.sign.push_back(sign);
        r_.shares.push_back(shares);
        r_.notional.push_back(shares * (price / 1e4));
        r_.mid_before.push_back(before);
        r_.mid_after.push_back(before);
    }

    std::uint64_t start_, end_, ts_ = 0;
    book::TickBook<> b_;
    book::ItchApply<book::TickBook<>> apply_{b_};
    std::unordered_map<std::uint64_t, std::uint32_t> mpid_;
    bool open_ = false;
    TradeRecord r_;
};

}  // namespace hft::strategy
