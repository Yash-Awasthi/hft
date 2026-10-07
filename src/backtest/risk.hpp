#pragma once

// Pre-trade checks on every order: position limit including open orders, order size, price
// collar around the mid, message-rate throttle (token bucket) and a loss kill switch.

#include <algorithm>
#include <array>
#include <cstdint>

#include "book/types.hpp"

namespace hft::backtest {

struct RiskLimits {
    std::int64_t max_position = 1000;        // shares, either side, open orders included
    std::uint32_t max_order = 500;           // shares
    std::uint32_t collar_ticks = 20;         // from the mid
    double max_msgs_per_s = 50;              // sustained message rate
    double burst = 20;                       // token bucket depth
    std::int64_t kill_loss = 5'000'000'000;  // micro-dollars ($5,000)
};

enum class Reject : std::uint8_t { None, Position, Size, Collar, Throttle, Killed };

class Risk {
   public:
    explicit Risk(RiskLimits l = {}) : l_(l), tokens_(l.burst) {}

    // Orders; cancels pass every check but the throttle.
    Reject check_order(book::Side side, std::uint32_t price, std::uint32_t qty,
                       std::int64_t inventory, std::int64_t open_buy, std::int64_t open_sell,
                       double mid_px, std::uint32_t tick, std::uint64_t now, std::int64_t pnl) {
        Reject r = Reject::None;
        if (killed_ || pnl < -l_.kill_loss) {
            killed_ = true;
            r = Reject::Killed;
        } else if (qty == 0 || qty > l_.max_order) {
            r = Reject::Size;
        } else if (side == book::Side::Buy
                       ? inventory + open_buy + qty > l_.max_position
                       : inventory - open_sell - static_cast<std::int64_t>(qty) <
                             -l_.max_position) {
            r = Reject::Position;
        } else if (std::abs(price - mid_px) > l_.collar_ticks * static_cast<double>(tick)) {
            r = Reject::Collar;
        } else if (!take_token(now)) {
            r = Reject::Throttle;
        }
        ++counts_[static_cast<std::size_t>(r)];
        return r;
    }

    bool check_cancel(std::uint64_t now) { return take_token(now); }

    bool killed() const { return killed_; }
    std::uint64_t count(Reject r) const { return counts_[static_cast<std::size_t>(r)]; }

   private:
    bool take_token(std::uint64_t now) {
        if (last_ && now > last_)
            tokens_ = std::min(l_.burst, tokens_ + (now - last_) * 1e-9 * l_.max_msgs_per_s);
        last_ = std::max(last_, now);
        if (tokens_ < 1) return false;
        tokens_ -= 1;
        return true;
    }

    RiskLimits l_;
    double tokens_;
    std::uint64_t last_ = 0;
    bool killed_ = false;
    std::array<std::uint64_t, 6> counts_{};
};

}  // namespace hft::backtest
