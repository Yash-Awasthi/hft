#pragma once

#include <cstdint>

namespace hft::book {

// Order references must be below this (ITCH assigns about 10^9 a day); the ID maps pack them
// into 40 bits.
inline constexpr std::uint64_t kMaxRef = (1ull << 40) - 1;
// Live orders per book are below this; the ID maps pack order indices into 24 bits.
inline constexpr std::uint32_t kMaxOrders = 1u << 24;

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
