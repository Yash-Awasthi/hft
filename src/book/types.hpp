#pragma once

#include <cstdint>

namespace hft::book {

enum class Side : std::uint8_t { Buy, Sell };

// Empty side: price 0 and quantity 0.
struct Bbo {
    std::uint32_t bid_px = 0;
    std::uint32_t ask_px = 0;
    std::uint64_t bid_qty = 0;
    std::uint64_t ask_qty = 0;
    bool operator==(const Bbo&) const = default;
};

}  // namespace hft::book
