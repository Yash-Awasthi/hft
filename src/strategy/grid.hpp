#pragma once

// Regular time grid over one symbol for estimating the quoting MDP (DESIGN.md section 3).
// At the start of each step: best prices and sizes and the feature row; during the step, per
// side, the shares executed and cancelled at the starting best price, and whether that price
// stopped being the best (moved through or away); at the end, the mid.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "data/store.hpp"
#include "feed/itch.hpp"
#include "strategy/multi_features.hpp"

namespace hft::strategy {

struct GridStep {
    std::uint64_t ts;
    std::uint32_t bid_px, ask_px;
    double bid_qty, ask_qty;
    double exec[2] = {0, 0}, cancel[2] = {0, 0};  // at the starting best: [0] bid, [1] ask
    std::int8_t moved[2] = {0, 0};  // +1 the best improved past it, -1 it was taken out, 0 held
    double mid_end = std::numeric_limits<double>::quiet_NaN();
};

class GridSampler {
   public:
    GridSampler(std::uint64_t step_ns, std::uint64_t start_ns, std::uint64_t end_ns)
        : step_(step_ns), start_(start_ns), end_(end_ns) {}

    // target: the traded symbol's locate; index: cross-asset inputs for the feature rows.
    void run(const std::string& store, std::uint16_t target, std::vector<std::uint16_t> index) {
        std::vector<std::uint16_t> all{target};
        all.insert(all.end(), index.begin(), index.end());
        std::vector<std::size_t> idx_slots;
        for (std::size_t i = 1; i < all.size(); ++i) idx_slots.push_back(i);
        MultiFeatures mf(all.size(), idx_slots);
        k_ = mf.count();
        data::MergedReader rd(store, all, 64);
        data::Record rec{};
        std::uint16_t loc;
        std::uint64_t next = start_;
        bool open = false;
        GridStep cur{};
        while (rd.next(rec, loc)) {
            const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
            if (ts >= end_) break;
            // Close every step boundary passed before this message.
            while (ts >= next && next < end_) {
                const book::Bbo b = mf.book(0).bbo();
                if (open) {
                    cur.mid_end = mid(b);
                    steps_.push_back(cur);
                }
                open = b.bid_px && b.ask_px && b.ask_px > b.bid_px && next >= start_;
                if (open) {
                    cur = GridStep{next, b.bid_px, b.ask_px, static_cast<double>(b.bid_qty),
                                   static_cast<double>(b.ask_qty)};
                    const std::size_t at = rows_.size();
                    rows_.resize(at + k_);
                    mf.row(0, next, rows_.data() + at);
                }
                next += step_;
            }
            const std::size_t slot =
                loc == target ? 0
                              : 1 + static_cast<std::size_t>(
                                        std::find(index.begin(), index.end(), loc) - index.begin());
            if (!mf.on_itch(slot, rec.data, rec.len, rec.seq) || slot != 0 || !open) continue;
            const MarketEvent& e = mf.last_event();
            const int s = e.side == book::Side::Buy ? 0 : 1;
            const std::uint32_t px = s == 0 ? cur.bid_px : cur.ask_px;
            if (e.price == px && e.kind != EventKind::Hidden && e.kind != EventKind::Add) {
                if (e.kind == EventKind::Execute) cur.exec[s] += e.shares;
                if (e.kind == EventKind::Cancel || e.kind == EventKind::Delete ||
                    e.kind == EventKind::Replace)
                    cur.cancel[s] += e.shares;
            }
            const book::Bbo b = mf.book(0).bbo();
            for (int side = 0; side < 2; ++side) {
                if (cur.moved[side]) continue;
                const std::uint32_t start_px = side == 0 ? cur.bid_px : cur.ask_px;
                const std::uint32_t now_px = side == 0 ? b.bid_px : b.ask_px;
                if (now_px == start_px || !now_px) continue;
                const bool better = side == 0 ? now_px > start_px : now_px < start_px;
                cur.moved[side] = better ? 1 : -1;
            }
        }
        if (open) {
            cur.mid_end = mid(mf.book(0).bbo());
            steps_.push_back(cur);
        }
    }

    const std::vector<GridStep>& steps() const { return steps_; }
    const std::vector<double>& rows() const { return rows_; }
    std::size_t features() const { return k_; }

   private:
    static double mid(const book::Bbo& b) {
        return b.bid_px && b.ask_px && b.ask_px > b.bid_px
                   ? (static_cast<double>(b.bid_px) + b.ask_px) / 200.0
                   : std::numeric_limits<double>::quiet_NaN();
    }

    std::uint64_t step_, start_, end_;
    std::size_t k_ = 0;
    std::vector<GridStep> steps_;
    std::vector<double> rows_;
};

}  // namespace hft::strategy
