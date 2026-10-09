#pragma once

// Streaming features of DESIGN.md section 1, one SymbolFeatures per symbol. Every update is
// O(1): event-time EMAs, clock-time decaying sums, and a walk of the first five levels for
// order flow imbalance. on_event sees the book after an event and the event itself, nothing
// later, so features cannot use the future; targets come only from the separate labeler.

#include <algorithm>
#include <array>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <limits>

#include "book/tick_book.hpp"
#include "strategy/event.hpp"

namespace hft::strategy {

struct FeatureParams {
    double event_halflife = 50;  // events
    double clock_tau_s = 1.0;    // seconds, flow and rate features
    double hawkes_beta = 10.0;   // 1/s, exponential kernel decay
    double hawkes_alpha = 1.0;   // jump per trade
    double hawkes_mu = 0.5;      // baseline intensity, 1/s
    std::uint32_t tick = 100;    // ITCH units per tick
    // Re-walk only the side of the book an event touched, and only when the event reaches the
    // cached top levels. Valid when every book change is reported as an event, as MarketFeed
    // does; tests that edit the book by hand leave it off.
    bool incremental_depth = false;
};

// Exponential moving average in event time.
struct EventEma {
    double v = 0;
    void add(double x, double w) { v += w * (x - v); }
};

// Exponentially decaying sum in clock time: value(t) = sum of x_i exp(-(t - t_i) / tau). The
// owner decays every sum with the same tau together, once per event (SymbolFeatures::advance),
// so a sum costs a multiply rather than an exp.
struct DecaySum {
    double v = 0;
    void add(double x) { v += x; }
};

class SymbolFeatures {
   public:
    static constexpr std::size_t kLevels = 5;
    static constexpr std::array<const char*, 23> kNames = {"spread_ticks",     "imbalance_l1",
                                                           "imbalance_l3",     "ofi_l1",
                                                           "ofi_l2",           "ofi_l3",
                                                           "ofi_l4",           "ofi_l5",
                                                           "ofi_l1_clock",     "cancel_rate_bid",
                                                           "cancel_rate_ask",  "add_cancel_log",
                                                           "log_since_spread", "queue_race",
                                                           "hawkes_imbalance", "trade_sign",
                                                           "aggressor_imb",    "log_last_trade",
                                                           "hidden_imb",       "realized_vol",
                                                           "time_of_day",      "auction_imbalance",
                                                           "mid_minus_ema"};
    static constexpr std::size_t kCount = kNames.size();
    using Values = std::array<double, kCount>;

    explicit SymbolFeatures(FeatureParams p = {})
        : p_(p),
          w_(1 - std::exp2(-1 / p.event_halflife)),
          tau_(p.clock_tau_s * 1e9),
          hawkes_tau_(1e9 / p.hawkes_beta) {
        values_.fill(std::numeric_limits<double>::quiet_NaN());
    }

    // Mid in ticks after the last event, NaN until both sides have quoted.
    double mid() const { return mid_; }
    const Values& values() const { return values_; }

    template <class Book>
    void on_event(const Book& b, const MarketEvent& e) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double tick = p_.tick;
        const std::uint64_t clock_before = clock_t_;
        const double f_tau = advance(e.ts);
        Level bid[kLevels], ask[kLevels];
        depth(b, e, bid, ask);

        // Order flow imbalance per level (Cont, Kukanov, Stoikov; Xu, Gould, Howison).
        // An empty ask level ranks as an infinite price, an empty bid as zero.
        auto ap = [](std::uint32_t px) { return px ? px : 0xffffffffu; };
        for (std::size_t k = 0; k < kLevels; ++k) {
            double ofi = 0;
            if (bid[k].px >= prev_bid_[k].px) ofi += bid[k].qty;
            if (bid[k].px <= prev_bid_[k].px) ofi -= prev_bid_[k].qty;
            if (ap(ask[k].px) <= ap(prev_ask_[k].px)) ofi -= ask[k].qty;
            if (ap(ask[k].px) >= ap(prev_ask_[k].px)) ofi += prev_ask_[k].qty;
            ofi_[k].add(ofi, w_);
            if (k == 0) ofi_clock_.add(ofi);
            prev_bid_[k] = bid[k];
            prev_ask_[k] = ask[k];
        }

        const bool two_sided = bid[0].px && ask[0].px && ask[0].px > bid[0].px;
        const double spread = two_sided ? (ask[0].px - bid[0].px) / tick : nan;
        double new_mid = mid_;
        if (two_sided) new_mid = (static_cast<double>(bid[0].px) + ask[0].px) / (2 * tick);
        if (std::isfinite(mid_) && std::isfinite(new_mid))
            vol_.add((new_mid - mid_) * (new_mid - mid_), w_);
        mid_ = new_mid;
        if (std::isfinite(mid_)) {
            // The clock's decay factor is the EMA's when both last moved at the same time.
            const double f_mid = mid_ema_t_ == clock_before
                                     ? f_tau
                                     : std::exp(-static_cast<double>(e.ts - mid_ema_t_) / tau_);
            mid_ema_.v = std::isfinite(mid_ema_.v) && mid_ema_t_ ? mid_ + (mid_ema_.v - mid_) * f_mid
                                                                 : mid_;
            mid_ema_t_ = e.ts;
        }
        if (spread != last_spread_ && !(std::isnan(spread) && std::isnan(last_spread_))) {
            last_spread_ = spread;
            spread_change_ts_ = e.ts;
        }

        const bool removes = e.kind == EventKind::Execute || e.kind == EventKind::Cancel ||
                             e.kind == EventKind::Delete || e.kind == EventKind::Replace;
        const bool buy_side = e.side == book::Side::Buy;
        if (e.at_best) {
            const bool cancel = e.kind == EventKind::Cancel || e.kind == EventKind::Delete ||
                                e.kind == EventKind::Replace;
            (buy_side ? cancel_bid_ : cancel_ask_).add(cancel ? 1 : 0, w_);
            adds_best_.add(e.kind == EventKind::Add ? 1 : 0, w_);
            cancels_best_.add(cancel ? 1 : 0, w_);
            if (removes) (buy_side ? depl_bid_ : depl_ask_).add(e.shares);
        }
        if (e.kind == EventKind::Execute || e.kind == EventKind::Hidden) {
            const double sign = e.aggressor;
            if (sign != 0) {
                trade_sign_.add(sign, w_);
                (sign > 0 ? hawkes_buy_ : hawkes_sell_).add(p_.hawkes_alpha);
            }
            aggr_signed_.add(sign * e.shares);
            aggr_total_.add(e.shares);
            last_trade_ = e.shares;
            if (e.kind == EventKind::Hidden) {
                hidden_signed_.add(sign * e.shares);
                hidden_total_.add(e.shares);
            }
        }
        if (e.kind == EventKind::Imbalance) {
            const double tot =
                static_cast<double>(e.paired) + std::fabs(static_cast<double>(e.imbalance));
            auction_ = tot > 0 ? e.imbalance / tot : 0;
        }

        Values& v = values_;
        v[0] = spread;
        v[1] = imbalance(bid[0].qty, ask[0].qty);
        v[2] =
            imbalance(bid[0].qty + bid[1].qty + bid[2].qty, ask[0].qty + ask[1].qty + ask[2].qty);
        for (std::size_t k = 0; k < kLevels; ++k) v[3 + k] = ofi_[k].v;
        v[8] = ofi_clock_.v;
        v[9] = cancel_bid_.v;
        v[10] = cancel_ask_.v;
        v[11] = std::log((adds_best_.v + 1e-3) / (cancels_best_.v + 1e-3));
        v[12] = std::log1p(static_cast<double>(e.ts - spread_change_ts_) * 1e-9);
        const double rb = depl_bid_.v, ra = depl_ask_.v;
        const double tb = bid[0].qty / (rb + 1), ta = ask[0].qty / (ra + 1);
        v[13] = tb + ta > 0 ? (tb - ta) / (tb + ta) : 0;
        const double lb = p_.hawkes_mu + hawkes_buy_.v;
        const double ls = p_.hawkes_mu + hawkes_sell_.v;
        v[14] = (lb - ls) / (lb + ls);
        v[15] = trade_sign_.v;
        const double at = aggr_total_.v;
        v[16] = at > 0 ? aggr_signed_.v / at : 0;
        v[17] = std::log1p(last_trade_);
        const double ht = hidden_total_.v;
        v[18] = ht > 0 ? hidden_signed_.v / ht : 0;
        v[19] = std::sqrt(vol_.v);
        v[20] = (static_cast<double>(e.ts) - 34'200e9) / 23'400e9;
        v[21] = auction_;
        v[22] = std::isfinite(mid_) ? mid_ - mid_ema_.v : nan;
    }

   private:
    struct Level {
        std::uint32_t px = 0;
        double qty = 0;
    };

    // Decays every clock-time sum to `now`, one exp per time constant. Returns the factor of
    // clock_tau_s (1 when the clock does not move).
    double advance(std::uint64_t now) {
        if (now <= clock_t_) return 1;
        const double dt = static_cast<double>(now - clock_t_);
        const double f = std::exp(-dt / tau_), fh = std::exp(-dt / hawkes_tau_);
        for (DecaySum* s : {&ofi_clock_, &depl_bid_, &depl_ask_, &aggr_signed_, &aggr_total_,
                            &hidden_signed_, &hidden_total_})
            s->v *= f;
        hawkes_buy_.v *= fh;
        hawkes_sell_.v *= fh;
        clock_t_ = now;
        return f;
    }

    // The first levels of both sides after the event. With incremental_depth only the side
    // the event touched is walked again, and only when the event's price is not strictly
    // worse than the cached last level; every other kind of event leaves the book as it was.
    template <class Book>
    void depth(const Book& b, const MarketEvent& e, Level (&bid)[kLevels], Level (&ask)[kLevels]) {
        bool walk_bid = true, walk_ask = true;
        if (p_.incremental_depth && have_depth_) {
            walk_bid = walk_ask = false;
            switch (e.kind) {
                case EventKind::Add:
                case EventKind::Execute:
                case EventKind::Cancel:
                case EventKind::Delete:
                    if (e.side == book::Side::Buy)
                        walk_bid = !prev_bid_[kLevels - 1].px || e.price >= prev_bid_[kLevels - 1].px;
                    else
                        walk_ask = !prev_ask_[kLevels - 1].px || e.price <= prev_ask_[kLevels - 1].px;
                    break;
                case EventKind::Replace:  // the event carries only the old price
                    walk_bid = walk_ask = true;
                    break;
                case EventKind::Hidden:
                case EventKind::Cross:
                case EventKind::Imbalance:
                case EventKind::Other: break;
            }
        }
        if (walk_bid)
            walk(b, book::Side::Buy, bid);
        else
            std::copy(std::begin(prev_bid_), std::end(prev_bid_), bid);
        if (walk_ask)
            walk(b, book::Side::Sell, ask);
        else
            std::copy(std::begin(prev_ask_), std::end(prev_ask_), ask);
        have_depth_ = true;
    }

    static double imbalance(double b, double a) { return b + a > 0 ? (b - a) / (b + a) : 0; }

    template <class Book>
    static void walk(const Book& b, book::Side s, Level (&out)[kLevels]) {
        auto f = b.best_level(s);
        for (std::size_t k = 0; k < kLevels; ++k) {
            if (!f) {
                out[k] = {};
                continue;
            }
            out[k] = {f->price, static_cast<double>(f->qty)};
            f = b.next_level_info(s, f->price);
        }
    }

    FeatureParams p_;
    double w_, tau_, hawkes_tau_;
    std::uint64_t clock_t_ = 0;  // time every DecaySum has been decayed to
    bool have_depth_ = false;
    Values values_;
    Level prev_bid_[kLevels]{}, prev_ask_[kLevels]{};
    EventEma ofi_[kLevels]{}, cancel_bid_, cancel_ask_, adds_best_, cancels_best_, trade_sign_,
        vol_;
    DecaySum ofi_clock_, depl_bid_, depl_ask_, hawkes_buy_, hawkes_sell_, aggr_signed_, aggr_total_,
        hidden_signed_, hidden_total_;
    EventEma mid_ema_{std::numeric_limits<double>::quiet_NaN()};
    std::uint64_t mid_ema_t_ = 0;
    double mid_ = std::numeric_limits<double>::quiet_NaN();
    double last_spread_ = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t spread_change_ts_ = 0;
    double last_trade_ = 0;
    double auction_ = 0;
};

}  // namespace hft::strategy
