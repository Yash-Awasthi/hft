#pragma once

// Lifecycles of real orders that join the best quote, for the fill model of DESIGN.md
// section 2. Each sampled order ends by its first execution (Fill), by a better price
// appearing on its side so it no longer stands at the touch (Away), or is censored: its own
// cancel, delete or replace, or the horizon. Covariates are taken at arrival.

#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"
#include "strategy/features.hpp"
#include "strategy/market_feed.hpp"

namespace hft::strategy {

enum class Outcome : std::uint8_t { Fill = 1, Away = 2, Censored = 0 };

struct Lifecycle {
    std::uint64_t ref, ts;
    std::int8_t side;  // +1 bid, -1 ask
    std::uint32_t price, shares;
    double queue_ahead, opposite_qty, imbalance, spread_ticks, volatility, signal;
    double duration_s;
    Outcome outcome;
};

// Per fill of a sampled order: markouts theta * (m(t + tau) - p) in ticks.
struct FillMark {
    std::uint64_t ref, ts;
    std::int8_t side;
    std::uint32_t price, shares;
    bool sweep;         // the level emptied within the same timestamp
    bool cancel_after;  // the order was cancelled within 1 ms after this fill
    double queue_ahead_at_arrival;
    std::vector<double> markout;
};

class LifecycleTracker {
   public:
    LifecycleTracker(std::uint64_t sample_one_in, double horizon_s, std::vector<double> taus_s)
        : n_(sample_one_in),
          horizon_ns_(static_cast<std::uint64_t>(horizon_s * 1e9)),
          taus_(std::move(taus_s)) {}

    void run(const std::string& store, std::uint16_t locate) {
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        while (rd.next(rec)) step(rec.data, rec.len, rec.seq);
        finish();
    }

    void step(const std::uint8_t* data, std::size_t len, std::uint64_t seq) {
        const char type = static_cast<char>(data[0]);
        const std::uint64_t ts = itch::detail::read_header(data).timestamp;
        expire(ts);
        const book::Bbo before = b_.bbo();
        MarketEvent e;
        const std::uint64_t ref = type == 'A' || type == 'F' || type == 'E' || type == 'C' ||
                                          type == 'X' || type == 'D' || type == 'U'
                                      ? load_be64(data + 11)
                                      : 0;
        const double ahead_before = (type == 'A' || type == 'F') ? level_before(b_, data) : 0;
        if (!feed_.on_itch(data, len, seq, e)) return;
        f_.on_event(b_, e);
        mids_.push_back({ts, f_.mid()});
        const book::Bbo after = b_.bbo();
        if ((type == 'A' || type == 'F') && e.at_best && sampled(ref)) {
            const auto& v = f_.values();
            Lifecycle l{
                ref,
                ts,
                static_cast<std::int8_t>(e.side == book::Side::Buy ? 1 : -1),
                e.price,
                e.shares,
                ahead_before,
                static_cast<double>(e.side == book::Side::Buy ? after.ask_qty : after.bid_qty),
                v[1],
                v[0],
                v[19],
                v[3],
                0,
                Outcome::Censored};
            open_[ref] = lives_.size();
            lives_.push_back(l);
            by_price_[e.side == book::Side::Buy ? 0 : 1].emplace(e.price, ref);
        }
        if (const auto it = open_.find(ref); it != open_.end() && ref) {
            Lifecycle& l = lives_[it->second];
            if (e.kind == EventKind::Execute) {
                const bool level_gone =
                    l.side > 0 ? after.bid_px != l.price : after.ask_px != l.price;
                marks_.push_back(
                    {ref, ts, l.side, l.price, e.shares, level_gone, false, l.queue_ahead, {}});
                pending_marks_.push_back(marks_.size() - 1);
                if (l.outcome == Outcome::Censored && l.duration_s == 0)
                    close(it->second, ts, Outcome::Fill, false);
                last_fill_[ref] = {marks_.size() - 1, ts};
            } else if (e.kind == EventKind::Cancel || e.kind == EventKind::Delete ||
                       e.kind == EventKind::Replace) {
                if (const auto lf = last_fill_.find(ref);
                    lf != last_fill_.end() && ts - lf->second.second <= 1'000'000)
                    marks_[lf->second.first].cancel_after = true;
                if (e.kind != EventKind::Cancel || !b_.order(ref)) {
                    if (l.duration_s == 0) close(it->second, ts, Outcome::Censored, false);
                    forget(ref, l);
                }
            }
        }
        // A better price on a side moves every tracked order behind it away.
        if (after.bid_px > before.bid_px) away(0, after.bid_px, ts);
        if (after.ask_px && (before.ask_px == 0 || after.ask_px < before.ask_px))
            away(1, after.ask_px, ts);
        resolve_marks(ts);
    }

    void finish() {
        expire(~0ull);
        resolve_marks(~0ull);
    }

    const std::vector<Lifecycle>& lifecycles() const { return lives_; }
    const std::vector<FillMark>& fills() const { return marks_; }

   private:
    bool sampled(std::uint64_t ref) const { return (ref * 0x9E3779B97F4A7C15ull >> 40) % n_ == 0; }

    static double level_before(const book::TickBook<>& b, const std::uint8_t* m) {
        const book::Side s = m[19] == 'B' ? book::Side::Buy : book::Side::Sell;
        return static_cast<double>(b.level_qty(s, load_be32(m + 32)));
    }

    void close(std::size_t i, std::uint64_t ts, Outcome o, bool) {
        Lifecycle& l = lives_[i];
        const std::uint64_t end = std::min(ts, l.ts + horizon_ns_);
        l.duration_s = std::max(1e-9, (end - l.ts) * 1e-9);
        l.outcome = ts <= l.ts + horizon_ns_ ? o : Outcome::Censored;
    }

    void forget(std::uint64_t ref, const Lifecycle& l) {
        auto& m = by_price_[l.side > 0 ? 0 : 1];
        for (auto [a, z] = m.equal_range(l.price); a != z; ++a)
            if (a->second == ref) {
                m.erase(a);
                break;
            }
        open_.erase(ref);
    }

    void away(int s, std::uint32_t best, std::uint64_t ts) {
        auto& m = by_price_[s];
        auto first = s == 0 ? m.begin() : m.upper_bound(best);
        auto last = s == 0 ? m.lower_bound(best) : m.end();
        std::vector<std::uint64_t> refs;
        for (auto it = first; it != last; ++it) refs.push_back(it->second);
        for (std::uint64_t r : refs) {
            const auto it = open_.find(r);
            if (it == open_.end()) continue;
            Lifecycle& l = lives_[it->second];
            if (l.duration_s == 0) close(it->second, ts, Outcome::Away, false);
            forget(r, l);
        }
    }

    void expire(std::uint64_t ts) {
        while (expire_from_ < lives_.size() && lives_[expire_from_].ts + horizon_ns_ < ts) {
            Lifecycle& l = lives_[expire_from_];
            if (l.duration_s == 0) {
                l.duration_s = horizon_ns_ * 1e-9;
                l.outcome = Outcome::Censored;
            }
            ++expire_from_;
        }
    }

    // Markouts need the mid after each tau; mids_ grows in time order.
    void resolve_marks(std::uint64_t now) {
        std::size_t keep = 0;
        for (std::size_t k = 0; k < pending_marks_.size(); ++k) {
            FillMark& m = marks_[pending_marks_[k]];
            const std::uint64_t last = m.ts + static_cast<std::uint64_t>(taus_.back() * 1e9);
            if (now <= last && now != ~0ull) {
                pending_marks_[keep++] = pending_marks_[k];
                continue;
            }
            for (double tau : taus_) {
                const std::uint64_t at = m.ts + static_cast<std::uint64_t>(tau * 1e9);
                double mid = std::nan("");
                if (at <= mids_.back().first) {
                    auto it = std::upper_bound(
                        mids_.begin(), mids_.end(), at,
                        [](std::uint64_t t, const auto& p) { return t < p.first; });
                    mid = std::prev(it)->second;
                }
                m.markout.push_back(m.side * (mid - m.price / 100.0));
            }
        }
        pending_marks_.resize(keep);
    }

    std::uint64_t n_, horizon_ns_;
    std::vector<double> taus_;
    std::vector<Lifecycle> lives_;
    std::vector<FillMark> marks_;
    std::vector<std::size_t> pending_marks_;
    std::unordered_map<std::uint64_t, std::size_t> open_;
    std::unordered_map<std::uint64_t, std::pair<std::size_t, std::uint64_t>> last_fill_;
    std::multimap<std::uint32_t, std::uint64_t> by_price_[2];
    std::vector<std::pair<std::uint64_t, double>> mids_;
    std::size_t expire_from_ = 0;
    book::TickBook<> b_{4096};
    MarketFeed<> feed_{b_};
    SymbolFeatures f_;
};

}  // namespace hft::strategy
