#pragma once

// Reference strategies: the sanity checks of DESIGN.md section 4 (zero, random taking,
// random passive, perfect foresight) and the naive join-the-best market maker.

#include <cmath>
#include <cstdint>
#include <vector>

#include "backtest/backtest.hpp"
#include "backtest/intensity.hpp"
#include "core/philox.hpp"

namespace hft::backtest {

struct Zero {
    void decide(const View&, Desired&) {}
};

// Joins the best bid and ask with a fixed size; stops adding to a side at the inventory cap.
struct NaiveJoin {
    std::uint32_t size = 100;
    std::int64_t max_inventory = 500;
    void decide(const View& v, Desired& d) {
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px) return;
        if (v.inventory < max_inventory) d.bid_px = b.bid_px, d.bid_qty = size;
        if (v.inventory > -max_inventory) d.ask_px = b.ask_px, d.ask_qty = size;
    }
};

// Takes `size` shares on a random side at a random fraction of events: loses about half the
// spread plus fees per share.
struct RandomTaker {
    rng::Stream rng{1, 0, 0};
    double rate = 0.001;
    std::uint32_t size = 100, draws = 0;
    void decide(const View& v, Desired& d) {
        const auto r = rng.draw(draws++, 0);
        if ((r[0] + 0.5) * 0x1p-32 >= rate) return;
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px) return;
        const bool buy = (r[1] & 1) && v.inventory < 500;
        const bool sell = !(r[1] & 1) && v.inventory > -500;
        if (buy) d.take_side = 1, d.take_qty = size, d.take_limit = b.ask_px;
        if (sell) d.take_side = -1, d.take_qty = size, d.take_limit = b.bid_px;
    }
};

// Quotes one random side at the best for a while, then the other: earns the spread net of
// adverse selection.
struct RandomPassive {
    rng::Stream rng{2, 0, 0};
    std::uint32_t size = 100, draws = 0;
    int side = 1;
    void decide(const View& v, Desired& d) {
        if (rng.draw(draws++, 0)[0] % 1000 == 0) side = -side;
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px) return;
        if (side > 0 && v.inventory < 500) d.bid_px = b.bid_px, d.bid_qty = size;
        if (side < 0 && v.inventory > -500) d.ask_px = b.ask_px, d.ask_qty = size;
    }
};

// Sees the mid `horizon` ahead (from the labeler's series) and takes when the move exceeds
// half the spread plus `cost_ticks`: an upper bound on what any signal can achieve.
struct PerfectForesight {
    std::vector<std::uint64_t> ts;
    std::vector<double> mid;  // ticks
    std::uint64_t horizon_ns = 1'000'000'000;
    double cost_ticks = 0.3;
    std::uint32_t size = 100;
    std::size_t i = 0, j = 0;
    double future(std::uint64_t at) {
        while (j + 1 < ts.size() && ts[j + 1] <= at) ++j;
        return j < mid.size() ? mid[j] : NAN;
    }
    void decide(const View& v, Desired& d) {
        if (!std::isfinite(v.mid)) return;
        const double ahead = future(v.now + horizon_ns) - v.mid;
        const book::Bbo b = v.book.bbo();
        const double half = (static_cast<double>(b.ask_px) - b.bid_px) / 200.0;
        if (ahead > half + cost_ticks && v.inventory < 500)
            d.take_side = 1, d.take_qty = size, d.take_limit = b.ask_px;
        else if (-ahead > half + cost_ticks && v.inventory > -500)
            d.take_side = -1, d.take_qty = size, d.take_limit = b.bid_px;
    }
};

}  // namespace hft::backtest

namespace hft::backtest {

// Avellaneda-Stoikov baseline (DESIGN.md section 3), prices in ticks. Reservation price
// r = m - q gamma sigma^2 (T - t) and distance d = ln(1 + gamma/k)/gamma + gamma sigma^2 (T - t)/2;
// with `glft` the Gueant-Lehalle-Fernandez-Tapia asymptotic quotes for bounded inventory
// replace them. sigma^2 (ticks^2 per second) is an EWMA of squared mid changes over time; A
// and k (fill intensity A exp(-k d), per second and per tick) come from the fill model, and
// with `online` are refitted after each of our fills from the exposure and fills by distance.
struct AvellanedaStoikov {
    double gamma = 0.1, A = 1.0, k = 1.5;
    bool glft = true;
    bool online = false;
    double max_dist_ticks = 0;  // caps both quote distances when positive; keep it inside the risk collar
    double online_tau_s = 600, online_prior_s = 60;
    std::uint32_t size = 100;
    std::int64_t max_inventory = 500;
    std::uint64_t end_ns = 57'600'000'000'000;
    std::uint32_t tick = 100;
    double var_tau_s = 60;

    double var = 0.0, last_mid = NAN;
    std::uint64_t last_ts = 0;
    IntensityEstimator est;
    double bid_dist = NAN, ask_dist = NAN;  // ticks from the mid of the last quotes, NaN if none
    std::int64_t last_inv = 0;
    std::uint64_t last_decide = 0;

    // Our fills show as inventory changes, credited to the distance of the side's last quote.
    void recalibrate(const View& v) {
        if (!last_decide) est = IntensityEstimator(A, k, online_tau_s, online_prior_s);
        const double dt = last_decide && v.now > last_decide ? (v.now - last_decide) * 1e-9 : 0;
        est.decay(dt);
        if (std::isfinite(bid_dist)) est.expose(bid_dist, dt);
        if (std::isfinite(ask_dist)) est.expose(ask_dist, dt);
        const std::int64_t dq = v.inventory - last_inv;
        const double lots = static_cast<double>(dq < 0 ? -dq : dq) / size;
        if (dq > 0 && std::isfinite(bid_dist)) est.fill(bid_dist, lots);
        if (dq < 0 && std::isfinite(ask_dist)) est.fill(ask_dist, lots);
        if (dq) est.fit(), A = est.A(), k = est.k();
        last_inv = v.inventory, last_decide = v.now;
        bid_dist = ask_dist = NAN;
    }

    void decide(const View& v, Desired& d) {
        if (online) recalibrate(v);
        if (!std::isfinite(v.mid)) return;
        if (std::isfinite(last_mid) && v.now > last_ts) {
            const double dt = (v.now - last_ts) * 1e-9;
            const double w = 1 - std::exp(-dt / var_tau_s);
            const double dm = v.mid - last_mid;
            var += w * (dm * dm / std::max(dt, 1e-3) - var);
        }
        if (v.mid != last_mid || !last_ts) last_mid = v.mid, last_ts = v.now;
        const double q = static_cast<double>(v.inventory) / size;
        double bid_d, ask_d;
        if (glft) {
            const double base = std::log1p(gamma / k) / gamma;
            const double c =
                std::sqrt(var * gamma / (2 * k * A) * std::pow(1 + gamma / k, 1 + k / gamma));
            bid_d = base + (2 * q + 1) / 2 * c;
            ask_d = base - (2 * q - 1) / 2 * c;
        } else {
            const double tl = v.now < end_ns ? (end_ns - v.now) * 1e-9 : 0;
            const double r_off = -q * gamma * var * tl;
            const double half = std::log1p(gamma / k) / gamma + gamma * var * tl / 2;
            bid_d = half - r_off;
            ask_d = half + r_off;
        }
        if (max_dist_ticks > 0) bid_d = std::min(bid_d, max_dist_ticks), ask_d = std::min(ask_d, max_dist_ticks);
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px) return;
        // To ticks on the grid, never crossing the opposite best (orders are post-only).
        const double bid = std::floor(v.mid - bid_d), ask = std::ceil(v.mid + ask_d);
        const std::uint32_t bpx = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(std::max(bid, 1.0)) * tick, b.ask_px - tick);
        const std::uint32_t apx = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::max(ask, 1.0)) * tick, b.bid_px + tick);
        if (v.inventory < max_inventory)
            d.bid_px = bpx, d.bid_qty = size, bid_dist = v.mid - static_cast<double>(bpx) / tick;
        if (v.inventory > -max_inventory)
            d.ask_px = apx, d.ask_qty = size, ask_dist = static_cast<double>(apx) / tick - v.mid;
    }
};

}  // namespace hft::backtest

#include <fstream>
#include <stdexcept>
#include <string>

namespace hft::backtest {

// Serves a quoting policy solved offline by archive/research/run_dp.py (research-archive tag): one table lookup per decision
// (DESIGN.md section 3). State: inventory in lots, queue bucket of our bid and ask (from the
// shares ahead of our orders in the strategy's own feed), imbalance, spread and signal buckets.
struct DpPolicy {
    std::int32_t Q = 0, n_queue = 0, n_imb = 0, n_spread = 0, n_sig = 0, k = 0;
    std::vector<double> imb_edges, spread_edges, sig_edges, means, stds, weights;
    std::vector<std::uint8_t> table;
    std::uint32_t lot = 100, tick = 100;
    double last_signal = 0;  // predicted mid change over the next second, ticks

    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        char magic[8];
        f.read(magic, 8);
        if (!f || std::string(magic, 8) != "HFTDP001")
            throw std::runtime_error("not a policy table: " + path);
        std::int32_t h[6];
        f.read(reinterpret_cast<char*>(h), sizeof h);
        Q = h[0], n_queue = h[1], n_imb = h[2], n_spread = h[3], n_sig = h[4], k = h[5];
        auto doubles = [&](std::vector<double>& v, std::size_t n) {
            v.resize(n);
            f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * 8));
        };
        doubles(imb_edges, n_imb - 1);
        doubles(spread_edges, n_spread - 1);
        doubles(sig_edges, n_sig - 1);
        doubles(means, k);
        doubles(stds, k);
        doubles(weights, k);
        table.resize(static_cast<std::size_t>(2 * Q + 1) * n_queue * n_imb * n_spread * n_sig);
        f.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
        if (!f) throw std::runtime_error("truncated policy table: " + path);
    }

    static int bucket(const std::vector<double>& edges, double x) {
        int b = 0;
        while (b < static_cast<int>(edges.size()) && x > edges[b]) ++b;
        return b;
    }

    // Matches archive/research/dp.py (research-archive tag): front at one lot or a third of the level ahead, then thirds.
    static int queue(const Working* w) {
        if (!w) return 0;
        const double rho = w->level > 0 ? w->ahead / w->level : 0;
        if (rho <= 1.0 / 3 || w->ahead <= 100) return 1;
        return rho <= 2.0 / 3 ? 2 : 3;
    }

    void decide(const View& v, Desired& d) {
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px ||
            v.n_features < static_cast<std::size_t>(k))
            return;
        const double spread = static_cast<double>(b.ask_px - b.bid_px) / tick;
        double sig = 0;
        for (std::int32_t i = 0; i < k; ++i) {
            const double x = std::isfinite(v.features[i]) ? v.features[i] : 0;
            sig += weights[i] * std::clamp((x - means[i]) / stds[i], -5.0, 5.0);
        }
        last_signal = sig;
        const double imb = std::isfinite(v.features[1]) ? v.features[1] : 0;
        const int x = (bucket(imb_edges, imb) * n_spread + bucket(spread_edges, spread)) * n_sig +
                      bucket(sig_edges, sig);
        const Working *wb = nullptr, *wa = nullptr;
        for (const Working& w : v.working) {
            if (w.ioc || w.cancel_sent) continue;
            if (w.side == book::Side::Buy && w.price == b.bid_px) wb = &w;
            if (w.side == book::Side::Sell && w.price == b.ask_px) wa = &w;
        }
        const int q = static_cast<int>(
            std::clamp<std::int64_t>(v.inventory / static_cast<std::int64_t>(lot), -Q, Q));
        const int s = (queue(wb) * 4 + queue(wa)) * n_imb * n_spread * n_sig + x;
        const int a =
            table[static_cast<std::size_t>(q + Q) * n_queue * n_imb * n_spread * n_sig + s];
        if (a == 9) {
            d.take_side = 1, d.take_qty = lot, d.take_limit = b.ask_px;
            return;
        }
        if (a == 10) {
            d.take_side = -1, d.take_qty = lot, d.take_limit = b.bid_px;
            return;
        }
        const int ab = a / 3, aa = a % 3;
        const bool room = b.ask_px - b.bid_px >= 2 * tick;
        if (ab) d.bid_px = ab == 2 && room ? b.bid_px + tick : b.bid_px, d.bid_qty = lot;
        if (aa) d.ask_px = aa == 2 && room ? b.ask_px - tick : b.ask_px, d.ask_qty = lot;
    }
};

}  // namespace hft::backtest

namespace hft::backtest {

// Strategy 5: the DP policy with signals plus extensions of DESIGN.md section 3, each one
// switchable for the ablation table. Toxicity guard: no resting quotes while realized
// volatility (feature 19, ticks per event) is above its limit. Aggressive taking: cross when
// the signal exceeds half the spread plus the taker fee and a margin.
struct Extended {
    DpPolicy dp;
    bool toxicity = true, taking = true;
    double vol_limit = 1.0, fee_ticks = 0.3, take_margin = 0.1;
    std::int64_t max_inventory = 500;

    void decide(const View& v, Desired& d) {
        dp.decide(v, d);
        if (toxicity && v.n_features > 19 && std::isfinite(v.features[19]) &&
            v.features[19] > vol_limit)
            d.bid_qty = d.ask_qty = 0;
        if (!taking || d.take_side) return;
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px) return;
        const double need = (b.ask_px - b.bid_px) / (2.0 * dp.tick) + fee_ticks + take_margin;
        if (dp.last_signal > need && v.inventory < max_inventory)
            d.take_side = 1, d.take_qty = dp.lot, d.take_limit = b.ask_px;
        else if (-dp.last_signal > need && v.inventory > -max_inventory)
            d.take_side = -1, d.take_qty = dp.lot, d.take_limit = b.bid_px;
    }
};

}  // namespace hft::backtest
