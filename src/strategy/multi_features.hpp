#pragma once

// Features for a set of symbols fed in feed order, with cross-asset features from index
// symbols (SPY, QQQ): each index's mid against its own clock-time EMA, in basis points, at
// the moment the target symbol's event happens. The same object serves batch export and the
// live loop, so the two cannot diverge.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "book/tick_book.hpp"
#include "strategy/features.hpp"
#include "strategy/market_feed.hpp"

namespace hft::strategy {

class MultiFeatures {
   public:
    static constexpr std::array<double, 2> kIndexTau = {0.1, 1.0};  // seconds
    static constexpr std::size_t kPerIndex = kIndexTau.size();

    // Symbols are identified by a dense slot; index_slots name the slots used as indices.
    MultiFeatures(std::size_t slots, std::vector<std::size_t> index_slots, FeatureParams p = {})
        : index_(std::move(index_slots)),
          ema_(index_.size()),
          held_(index_.size(), std::numeric_limits<double>::quiet_NaN()) {
        p.incremental_depth = true;  // every book change reaches the features as an event
        for (std::size_t i = 0; i < slots; ++i) sym_.push_back(std::make_unique<Sym>(p));
        for (auto& e : ema_) e.fill({std::numeric_limits<double>::quiet_NaN(), 0});
    }

    std::size_t count() const { return SymbolFeatures::kCount + index_.size() * kPerIndex; }

    // Applies one message of symbol `slot`; true when it changed the book or carried a trade,
    // in which case row() describes that symbol now.
    bool on_itch(std::size_t slot, const std::uint8_t* msg, std::size_t len, std::uint64_t seq) {
        Sym& s = *sym_[slot];
        if (!s.feed.on_itch(msg, len, seq, last_)) return false;
        s.f.on_event(s.book, last_);
        for (std::size_t k = 0; k < index_.size(); ++k) {
            if (index_[k] != slot || !std::isfinite(s.f.mid())) continue;
            for (std::size_t j = 0; j < kPerIndex; ++j) {
                Ema& e = ema_[k][j];
                // The mid held since the last index event decays into the EMA up to now.
                e.v = std::isfinite(e.v)
                          ? held_[k] + (e.v - held_[k]) * decay(last_.ts, e.t, kIndexTau[j])
                          : s.f.mid();
                e.t = last_.ts;
            }
            held_[k] = s.f.mid();
        }
        return true;
    }

    const MarketEvent& last_event() const { return last_; }
    const SymbolFeatures& symbol(std::size_t slot) const { return sym_[slot]->f; }
    const book::TickBook& book(std::size_t slot) const { return sym_[slot]->book; }

    // Own features of `slot`, then for each index and horizon the index momentum at time ts.
    void row(std::size_t slot, std::uint64_t ts, double* out) const {
        const auto& v = sym_[slot]->f.values();
        for (std::size_t i = 0; i < v.size(); ++i) out[i] = v[i];
        double* o = out + v.size();
        for (std::size_t k = 0; k < index_.size(); ++k) {
            const double m = held_[k];
            for (std::size_t j = 0; j < kPerIndex; ++j) {
                const Ema& e = ema_[k][j];
                const double at = m + (e.v - m) * decay(ts, e.t, kIndexTau[j]);
                *o++ = std::isfinite(at) && m > 0 ? (m - at) / m * 1e4 : 0;
            }
        }
    }

   private:
    struct Sym {
        explicit Sym(FeatureParams p) : feed(book), f(p) {}
        book::TickBook book;
        MarketFeed<> feed;
        SymbolFeatures f;
    };
    struct Ema {
        double v;
        std::uint64_t t;
    };

    static double decay(std::uint64_t now, std::uint64_t then, double tau_s) {
        return now > then ? std::exp(-static_cast<double>(now - then) / (tau_s * 1e9)) : 1;
    }

    std::vector<std::unique_ptr<Sym>> sym_;
    std::vector<std::size_t> index_;
    std::vector<std::array<Ema, kPerIndex>> ema_;
    std::vector<double> held_;  // current mid of each index
    MarketEvent last_;
};

}  // namespace hft::strategy
