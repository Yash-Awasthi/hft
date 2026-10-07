#pragma once

// Strategy accounting in integer micro-dollars, so the identity of DESIGN.md section 4
// holds exactly: total PnL (cash plus inventory at the mid) equals spread capture plus
// inventory PnL plus fee PnL after every event. Prices are ITCH units ($0.0001, 100
// micro-dollars) and mids half-units, both exact in micro-dollars.

#include <cstdint>
#include <deque>

namespace hft::backtest {

class Accounting {
   public:
    // The mid moves: inventory is marked to the new mid.
    void mid(std::uint32_t bid, std::uint32_t ask, std::uint64_t ts) {
        resolve(ts);
        const std::int64_t m = (static_cast<std::int64_t>(bid) + ask) * 50;
        if (have_mid_) inventory_pnl_ += inv_ * (m - mid_);
        mid_ = m;
        have_mid_ = true;
    }

    // side +1 buy, -1 sell; fee in micro-dollars paid (negative for a rebate received).
    void fill(int side, std::uint32_t price, std::int64_t shares, std::int64_t fee,
              std::uint64_t ts) {
        resolve(ts);
        const std::int64_t p = static_cast<std::int64_t>(price) * 100;
        cash_ -= side * p * shares + fee;
        inv_ += side * shares;
        spread_ += side * (mid_ - p) * shares;
        fees_ -= fee;
        ++fills_;
        volume_ += shares;
        pending_.push_back({ts, side * shares, mid_});
    }

    // Applies every adverse-selection term whose second has passed by `ts`.
    void flush() { resolve(~0ull); }

    std::int64_t inventory() const { return inv_; }
    std::int64_t cash() const { return cash_; }
    std::int64_t total_pnl() const { return cash_ + inv_ * mid_; }
    std::int64_t spread_capture() const { return spread_; }
    std::int64_t inventory_pnl() const { return inventory_pnl_; }
    std::int64_t fee_pnl() const { return fees_; }
    std::int64_t adverse_selection() const { return adverse_; }
    std::int64_t mid_micro() const { return mid_; }
    std::uint64_t fills() const { return fills_; }
    std::int64_t volume() const { return volume_; }
    bool identity_holds() const { return total_pnl() == spread_ + inventory_pnl_ + fees_; }

   private:
    // theta n (m(t + 1 s) - m(t)): the part of inventory PnL that follows each fill within 1 s.
    void resolve(std::uint64_t ts) {
        while (!pending_.empty() && (ts == ~0ull || pending_.front().ts + kWindow <= ts)) {
            const Pending& p = pending_.front();
            adverse_ += p.signed_shares * (mid_ - p.mid);
            pending_.pop_front();
        }
    }

    static constexpr std::uint64_t kWindow = 1'000'000'000;
    struct Pending {
        std::uint64_t ts;
        std::int64_t signed_shares;
        std::int64_t mid;
    };

    std::int64_t mid_ = 0;
    bool have_mid_ = false;
    std::int64_t cash_ = 0, inv_ = 0, spread_ = 0, inventory_pnl_ = 0, fees_ = 0, adverse_ = 0;
    std::uint64_t fills_ = 0;
    std::int64_t volume_ = 0;
    std::deque<Pending> pending_;
};

}  // namespace hft::backtest
