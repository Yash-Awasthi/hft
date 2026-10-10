#pragma once

#include <cstdint>

namespace hft::pm {

// A market's order rules and fee schedule as the metadata API states them. Units: tick in
// 1e-4 price units (0 = unknown, use the book's), min_qty in 1e-6 shares. Defaults are for
// recordings made before these were kept: the documented 5-share minimum and the general fee.
struct MarketRules {
    std::int32_t tick = 0;
    std::int64_t min_qty = 5'000'000;
    bool neg_risk = false;
    bool fees = true;  // takers pay rate * (p (1 - p))^exp per share
    std::uint32_t fee_rate_ppm = 50'000;
    std::uint8_t fee_exp = 1;
    std::uint16_t delay_ms = 0;  // matching delay for marketable orders
};

}  // namespace hft::pm
