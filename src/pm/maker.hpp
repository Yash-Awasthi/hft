#pragma once

// Market maker for one binary-outcome token, simulated on recorded level-2 data.
//
// Quotes follow Avellaneda-Stoikov in logit units: with x = ln(p / (1 - p)), inventory q in
// lots and belief variance s2 (an EWMA of squared logit mid changes per second),
//   reservation  r = x - q * gamma * s2 * tau
//   half-spread  d = gamma * s2 * tau / 2 + ln(1 + gamma / k) / gamma
// and the bid and ask are sigmoid(r - d) and sigmoid(r + d) on the price grid.
//
// Recorded data has no order identities, so fills come from trades only: a resting order
// has `ahead` shares in front of it (the level's displayed size when it was placed), a trade
// at its price reduces that first, and a trade through its price fills it. A level that
// shrinks below `ahead` pulls `ahead` down with it. Cancellations ahead of us are not
// credited, which makes fills conservative.

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>

#include "pm/book.hpp"

namespace hft::pm {

struct MakerParams {
    double gamma = 0.1, k = 30;
    double horizon_s = 3600;    // time left used in the inventory terms
    double size = 100;          // shares per quote
    double max_inv = 1000;      // shares; the side that would pass it is not quoted
    double min_mid = 0.05, max_mid = 0.95;
    double warmup_s = 30;       // no quotes until the variance has seen this much data
    double requote_s = 0.5;
    double var_tau_s = 60, var_prior = 1e-4;
};

// One fill of the maker's own order, for parity checks between fill models.
struct MakerFill {
    std::int64_t ns;
    std::uint32_t token;
    bool buy;
    std::int32_t px;
    double qty;
};

class TokenMaker {
   public:
    explicit TokenMaker(const MakerParams& p) : p_(p), var_(p.var_prior) {}

    TokenBook book;  // the driver applies exchange messages to this, then calls the matching on_*
    std::vector<MakerFill>* fill_log = nullptr;  // when set, every own fill is appended
    std::uint32_t token_index = 0;
    std::int32_t tick = 100;  // price grid, in 1e-4

    void on_snapshot(std::int64_t ns) {
        cancel(bid_), cancel(ask_);  // a new snapshot means we may have missed messages
        update(ns);
    }
    void on_level(std::int64_t ns) { update(ns); }

    // Risk switch: a disabled maker cancels its quotes and places none until enabled again.
    void set_enabled(bool on) {
        enabled_ = on;
        if (!on) cancel(bid_), cancel(ask_);
    }
    bool enabled() const { return enabled_; }

    // Fills come from a venue instead of this class's own model (exec::SimVenue): on_trade
    // then only requotes, and the venue's fills arrive through on_fill.
    void set_venue_fills(bool on) { venue_ = on; }

    // A fill of our own order on side `buy` at `px`, reported by the venue, before on_trade.
    // `current`: the fill belongs to the order this side holds now (not one already replaced).
    void on_fill(std::int64_t ns, bool buy, std::int32_t px, double qty, bool current) {
        if (qty > 0) book_fill(buy ? bid_ : ask_, !buy, qty, ns, px, current);
    }

    // `taker_buy`: the aggressor bought, so it consumes asks.
    void on_trade(std::int64_t ns, bool taker_buy, std::int32_t px, double size) {
        Order& o = taker_buy ? ask_ : bid_;
        if (!venue_ && o.px >= 0) {
            const bool through = taker_buy ? px > o.px : px < o.px;
            double fill = 0;
            if (through) {
                fill = o.rem;
            } else if (px == o.px) {
                if (size > o.ahead) fill = std::min(o.rem, size - o.ahead), o.ahead = 0;
                else o.ahead -= size;
            }
            if (fill > 0) book_fill(o, taker_buy, fill, ns, o.px, true);
        }
        update(ns);
    }

    double pnl() const { return cash_ + inv_ * last_mid_; }  // inventory at the last mid
    double mid() const { return last_mid_; }
    double inventory() const { return inv_; }
    double max_abs_inventory() const { return max_abs_inv_; }
    std::uint64_t buys() const { return buys_; }
    std::uint64_t sells() const { return sells_; }
    std::uint64_t quotes() const { return quotes_; }
    double shares_traded() const { return shares_; }
    double variance() const { return var_; }
    std::int32_t bid_px() const { return bid_.px; }
    std::int32_t ask_px() const { return ask_.px; }
    double size() const { return p_.size; }
    // Counts placements per side: a change means a new order, even at the same price (after a
    // full fill the maker places again at the same price).
    std::uint64_t bid_seq() const { return bid_.seq; }
    std::uint64_t ask_seq() const { return ask_.seq; }

   private:
    struct Order {
        std::int32_t px = -1;
        double rem = 0, ahead = 0;
        std::uint64_t seq = 0;
    };

    void book_fill(Order& o, bool taker_buy, double fill, std::int64_t ns, std::int32_t px, bool current) {
        if (fill_log) fill_log->push_back({ns, token_index, !taker_buy, px, fill});
        const double dollars = fill * px * 1e-4;
        cash_ += taker_buy ? dollars : -dollars;
        inv_ += taker_buy ? -fill : fill;
        max_abs_inv_ = std::max(max_abs_inv_, std::abs(inv_));
        (taker_buy ? sells_ : buys_) += 1;
        shares_ += fill;
        if (current && (o.rem -= fill) <= 0) o.px = -1;
    }

    static double logit(double p) { return std::log(p / (1 - p)); }
    static double sigmoid(double x) { return 1 / (1 + std::exp(-x)); }
    static void cancel(Order& o) { o.px = -1; }

    void place(Order& o, bool buy, std::int32_t px) {
        if (o.px == px) return;
        o.px = px, o.rem = p_.size, ++quotes_, ++o.seq;
        const std::int32_t best = buy ? book.best_bid() : book.best_ask();
        const bool inside = buy ? px > best : px < best;
        o.ahead = inside ? 0.0 : book.size_at(buy, px);
    }

    void update(std::int64_t ns) {
        if (!book.two_sided() || book.crossed()) {
            cancel(bid_), cancel(ask_);
            return;
        }
        const double mid = (book.best_bid() + book.best_ask()) * 0.5e-4;
        if (!start_ns_) start_ns_ = ns;
        if (last_mid_ > 0 && mid != last_mid_ && ns > last_ns_) {
            const double dt = static_cast<double>(ns - last_ns_) * 1e-9;
            const double dx = logit(mid) - logit(last_mid_);
            var_ += (1 - std::exp(-dt / p_.var_tau_s)) * (dx * dx / std::max(dt, 1e-3) - var_);
        }
        if (mid != last_mid_ || !last_ns_) last_mid_ = mid, last_ns_ = ns;
        for (Order* o : {&bid_, &ask_}) {  // a level cannot be deeper than what is displayed
            if (o->px >= 0) o->ahead = std::min(o->ahead, book.size_at(o == &bid_, o->px));
        }
        if (!enabled_ || static_cast<double>(ns - start_ns_) * 1e-9 < p_.warmup_s) return;
        if (mid < p_.min_mid || mid > p_.max_mid) {
            cancel(bid_), cancel(ask_);
            return;
        }
        if (static_cast<double>(ns - last_quote_ns_) * 1e-9 < p_.requote_s) return;
        last_quote_ns_ = ns;

        const double q = inv_ / p_.size;
        const double risk = p_.gamma * var_ * p_.horizon_s;
        const double r = logit(mid) - q * risk;
        const double d = risk / 2 + std::log1p(p_.gamma / p_.k) / p_.gamma;
        // Grid prices that never cross the opposite best (orders are post-only).
        const double bid_f = std::floor(sigmoid(r - d) * 1e4 / tick) * tick;
        const double ask_f = std::ceil(sigmoid(r + d) * 1e4 / tick) * tick;
        const std::int32_t bid = static_cast<std::int32_t>(std::min<double>(bid_f, book.best_ask() - tick));
        const std::int32_t ask = static_cast<std::int32_t>(std::max<double>(ask_f, book.best_bid() + tick));
        if (inv_ < p_.max_inv && bid >= tick) place(bid_, true, bid);
        else cancel(bid_);
        if (inv_ > -p_.max_inv && ask <= 10000 - tick) place(ask_, false, ask);
        else cancel(ask_);
    }

    MakerParams p_;
    Order bid_, ask_;
    double var_, last_mid_ = 0, cash_ = 0, inv_ = 0, max_abs_inv_ = 0, shares_ = 0;
    std::int64_t start_ns_ = 0, last_ns_ = 0, last_quote_ns_ = 0;
    std::uint64_t buys_ = 0, sells_ = 0, quotes_ = 0;
    bool enabled_ = true, venue_ = false;
};

}  // namespace hft::pm
