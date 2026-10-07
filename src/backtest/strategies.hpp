#pragma once

// Reference strategies: the sanity checks of DESIGN.md section 4 (zero, random taking,
// random passive, perfect foresight) and the naive join-the-best market maker.

#include <cmath>
#include <cstdint>
#include <vector>

#include "backtest/backtest.hpp"
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
// and k (fill intensity A exp(-k d), per second and per tick) come from the fill model.
struct AvellanedaStoikov {
    double gamma = 0.1, A = 1.0, k = 1.5;
    bool glft = true;
    std::uint32_t size = 100;
    std::int64_t max_inventory = 500;
    std::uint64_t end_ns = 57'600'000'000'000;
    std::uint32_t tick = 100;
    double var_tau_s = 60;

    double var = 0.0, last_mid = NAN;
    std::uint64_t last_ts = 0;

    void decide(const View& v, Desired& d) {
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
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px || b.ask_px <= b.bid_px) return;
        // To ticks on the grid, never crossing the opposite best (orders are post-only).
        const double bid = std::floor(v.mid - bid_d), ask = std::ceil(v.mid + ask_d);
        const std::uint32_t bpx = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(std::max(bid, 1.0)) * tick, b.ask_px - tick);
        const std::uint32_t apx = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::max(ask, 1.0)) * tick, b.bid_px + tick);
        if (v.inventory < max_inventory) d.bid_px = bpx, d.bid_qty = size;
        if (v.inventory > -max_inventory) d.ask_px = apx, d.ask_qty = size;
    }
};

}  // namespace hft::backtest
