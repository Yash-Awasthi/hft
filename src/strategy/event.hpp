#pragma once

// A market event as the strategy layer sees it: what happened, to which side and price,
// and where it sat relative to the best quote before the event. Built from ITCH by
// MarketFeed, which applies the message to the book; features never touch raw messages.

#include <cstdint>

#include "book/types.hpp"

namespace hft::strategy {

enum class EventKind : std::uint8_t {
    Add,
    Execute,
    Cancel,
    Delete,
    Replace,
    Hidden,
    Cross,
    Imbalance,
    Other
};

struct MarketEvent {
    EventKind kind = EventKind::Other;
    book::Side side = book::Side::Buy;  // side of the resting order (meaningless for Hidden)
    bool at_best = false;               // the order sat at the best level before the event
    std::uint32_t price = 0;            // order price, or trade price for Hidden and Cross
    std::uint32_t shares = 0;
    std::int8_t aggressor = 0;   // trades: +1 buyer-initiated, -1 seller-initiated, 0 unknown
    std::uint64_t ts = 0;        // ns since midnight
    std::uint64_t seq = 0;       // feed sequence
    std::int64_t imbalance = 0;  // signed NOII imbalance shares (Imbalance only)
    std::uint64_t paired = 0;
};

}  // namespace hft::strategy
