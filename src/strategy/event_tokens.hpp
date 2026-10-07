#pragma once

// Event tokens for the event transformer (DESIGN.md section 11): per book event of one symbol,
// type (add, execute, cancel or delete, replace, hidden), side (resting order; for hidden prints
// the side taken), distance of the price from the mid before the event in whole ticks (0 for
// the touch of a one-tick spread, capped at 9), size bucket (0 below 100 shares, then one per
// doubling, capped at 7), log1p of the microseconds since the previous event, and the mid after.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "strategy/market_feed.hpp"

namespace hft::strategy {

struct TokenRecord {
    std::vector<std::uint64_t> ts;
    std::vector<std::uint8_t> type, side, dist, size;
    std::vector<float> log_dt;
    std::vector<double> mid_after;  // ticks
};

class Tokenizer {
   public:
    static constexpr int kTypes = 5, kDist = 10, kSizes = 8;

    Tokenizer(std::uint64_t start_ns, std::uint64_t end_ns, std::uint32_t tick = 100)
        : start_(start_ns), end_(end_ns), tick_(tick) {}

    void run(const std::string& store, std::uint16_t locate) {
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        while (rd.next(rec)) on_itch(rec.data, rec.len, rec.seq);
    }

    void on_itch(const std::uint8_t* msg, std::size_t len, std::uint64_t seq) {
        const double before = mid();
        MarketEvent e;
        if (!feed_.on_itch(msg, len, seq, e)) return;
        int type;
        switch (e.kind) {
            case EventKind::Add: type = 0; break;
            case EventKind::Execute: type = 1; break;
            case EventKind::Cancel:
            case EventKind::Delete: type = 2; break;
            case EventKind::Replace: type = 3; break;
            case EventKind::Hidden: type = 4; break;
            default: return;
        }
        if (e.ts < start_ || e.ts >= end_ || !std::isfinite(before)) return;
        const int side = type == 4 ? (e.aggressor > 0 ? 1 : 0) : (e.side == book::Side::Buy ? 0 : 1);
        const double d = std::abs(e.price / static_cast<double>(tick_) - before);
        const int sz = e.shares < 100 ? 0 : std::min(7, 1 + static_cast<int>(std::log2(e.shares / 100.0)));
        r_.ts.push_back(e.ts);
        r_.type.push_back(static_cast<std::uint8_t>(type));
        r_.side.push_back(static_cast<std::uint8_t>(side));
        r_.dist.push_back(static_cast<std::uint8_t>(std::min(9.0, std::floor(d))));
        r_.size.push_back(static_cast<std::uint8_t>(sz));
        r_.log_dt.push_back(last_ ? static_cast<float>(std::log1p((e.ts - last_) * 1e-3)) : 0.0f);
        r_.mid_after.push_back(mid());
        last_ = e.ts;
    }

    const TokenRecord& record() const { return r_; }

   private:
    double mid() const {
        const book::Bbo q = b_.bbo();
        return q.bid_px && q.ask_px && q.ask_px > q.bid_px ? (q.bid_px + q.ask_px) / (2.0 * tick_) : NAN;
    }

    std::uint64_t start_, end_;
    std::uint32_t tick_;
    std::uint64_t last_ = 0;
    book::TickBook<> b_;
    MarketFeed<> feed_{b_};
    TokenRecord r_;
};

}  // namespace hft::strategy
