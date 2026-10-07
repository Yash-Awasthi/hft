#pragma once

// Streaming features of DESIGN.md section 1, one SymbolFeatures per symbol. Every update is
// O(1): event-time EMAs, clock-time decaying sums, and a walk of the first five levels for
// order flow imbalance. on_event sees the book after an event and the event itself, nothing
// later, so features cannot use the future; targets come only from the separate labeler.

#include <array>
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
};

// Exponential moving average in event time.
struct EventEma {
    double v = 0;
    void add(double x, double w) { v += w * (x - v); }
};

// Exponentially decaying sum in clock time: value(t) = sum of x_i exp(-(t - t_i) / tau).
struct DecaySum {
    double v = 0;
    std::uint64_t t = 0;
    double at(std::uint64_t now, double tau_ns) const {
        return now <= t ? v : v * std::exp(-static_cast<double>(now - t) / tau_ns);
    }
    void add(std::uint64_t now, double x, double tau_ns) {
        v = at(now, tau_ns) + x;
        t = now > t ? now : t;
    }
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
        : p_(p), w_(1 - std::exp2(-1 / p.event_halflife)), tau_(p.clock_tau_s * 1e9) {
        values_.fill(std::numeric_limits<double>::quiet_NaN());
    }

    // Mid in ticks after the last event, NaN until both sides have quoted.
    double mid() const { return mid_; }
    const Values& values() const { return values_; }

    template <class Book>
    void on_event(const Book& b, const MarketEvent& e) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double tick = p_.tick;
        Level bid[kLevels], ask[kLevels];
        walk(b, book::Side::Buy, bid);
        walk(b, book::Side::Sell, ask);

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
            if (k == 0) ofi_clock_.add(e.ts, ofi, tau_);
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
            mid_ema_.v = std::isfinite(mid_ema_.v) && mid_ema_t_
                             ? mid_ + (mid_ema_.v - mid_) *
                                          std::exp(-static_cast<double>(e.ts - mid_ema_t_) / tau_)
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
            if (removes) (buy_side ? depl_bid_ : depl_ask_).add(e.ts, e.shares, tau_);
        }
        if (e.kind == EventKind::Execute || e.kind == EventKind::Hidden) {
            const double sign = e.aggressor;
            if (sign != 0) {
                trade_sign_.add(sign, w_);
                (sign > 0 ? hawkes_buy_ : hawkes_sell_)
                    .add(e.ts, p_.hawkes_alpha, 1e9 / p_.hawkes_beta);
            }
            aggr_signed_.add(e.ts, sign * e.shares, tau_);
            aggr_total_.add(e.ts, e.shares, tau_);
            last_trade_ = e.shares;
            if (e.kind == EventKind::Hidden) {
                hidden_signed_.add(e.ts, sign * e.shares, tau_);
                hidden_total_.add(e.ts, e.shares, tau_);
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
        v[8] = ofi_clock_.at(e.ts, tau_);
        v[9] = cancel_bid_.v;
        v[10] = cancel_ask_.v;
        v[11] = std::log((adds_best_.v + 1e-3) / (cancels_best_.v + 1e-3));
        v[12] = std::log1p(static_cast<double>(e.ts - spread_change_ts_) * 1e-9);
        const double rb = depl_bid_.at(e.ts, tau_), ra = depl_ask_.at(e.ts, tau_);
        const double tb = bid[0].qty / (rb + 1), ta = ask[0].qty / (ra + 1);
        v[13] = tb + ta > 0 ? (tb - ta) / (tb + ta) : 0;
        const double lb = p_.hawkes_mu + hawkes_buy_.at(e.ts, 1e9 / p_.hawkes_beta);
        const double ls = p_.hawkes_mu + hawkes_sell_.at(e.ts, 1e9 / p_.hawkes_beta);
        v[14] = (lb - ls) / (lb + ls);
        v[15] = trade_sign_.v;
        const double at = aggr_total_.at(e.ts, tau_);
        v[16] = at > 0 ? aggr_signed_.at(e.ts, tau_) / at : 0;
        v[17] = std::log1p(last_trade_);
        const double ht = hidden_total_.at(e.ts, tau_);
        v[18] = ht > 0 ? hidden_signed_.at(e.ts, tau_) / ht : 0;
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

    static double imbalance(double b, double a) { return b + a > 0 ? (b - a) / (b + a) : 0; }

    template <class Book>
    static void walk(const Book& b, book::Side s, Level (&out)[kLevels]) {
        auto f = b.front(s);
        for (std::size_t k = 0; k < kLevels; ++k) {
            if (!f) {
                out[k] = {};
                continue;
            }
            out[k] = {f->price, static_cast<double>(b.level_qty(s, f->price))};
            f = b.next_level(s, f->price);
        }
    }

    FeatureParams p_;
    double w_, tau_;
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
