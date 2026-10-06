#pragma once

// Nasdaq TotalView-ITCH 5.0 decoder. Zero allocation: every message is read from the
// caller's buffer into a plain struct of host-order fields.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"

namespace hft::itch {

struct Header {
    char type;
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t timestamp;  // nanoseconds since midnight
};

struct SystemEvent {
    Header h;
    char event;
};

struct StockDirectory {
    Header h;
    char stock[8];
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    char issue_subtype[2];
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_tier;
    char etp_flag;
    std::uint32_t etp_leverage;
    char inverse;
};

struct TradingAction {
    Header h;
    char stock[8];
    char state;
    char reserved;
    char reason[4];
};

// 'A' and 'F'; mpid is zero-filled for 'A'.
struct AddOrder {
    Header h;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    char stock[8];
    std::uint32_t price;  // four implied decimals
    char mpid[4];
};

struct OrderExecuted {
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
};

struct OrderExecutedPrice {
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    char printable;
    std::uint32_t price;
};

struct OrderCancel {
    Header h;
    std::uint64_t ref;
    std::uint32_t cancelled;
};

struct OrderDelete {
    Header h;
    std::uint64_t ref;
};

struct OrderReplace {
    Header h;
    std::uint64_t orig_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

struct Trade {
    Header h;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    char stock[8];
    std::uint32_t price;
    std::uint64_t match;
};

struct CrossTrade {
    Header h;
    std::uint64_t shares;
    char stock[8];
    std::uint32_t price;
    std::uint64_t match;
    char cross_type;
};

struct BrokenTrade {
    Header h;
    std::uint64_t match;
};

struct Imbalance {
    Header h;
    std::uint64_t paired;
    std::uint64_t imbalance;
    char direction;
    char stock[8];
    std::uint32_t far_price;
    std::uint32_t near_price;
    std::uint32_t ref_price;
    char cross_type;
    char variation;
};

enum class Status { Ok, Truncated, BadLength, UnknownType };

// Total message length in bytes including the type byte; 0 for an unknown type.
constexpr std::size_t length_of(char type) {
    switch (type) {
        case 'S':
            return 12;
        case 'R':
            return 39;
        case 'H':
            return 25;
        case 'Y':
            return 20;
        case 'L':
            return 26;
        case 'V':
            return 35;
        case 'W':
            return 12;
        case 'K':
            return 28;
        case 'J':
            return 35;
        case 'h':
            return 21;
        case 'A':
            return 36;
        case 'F':
            return 40;
        case 'E':
            return 31;
        case 'C':
            return 36;
        case 'X':
            return 23;
        case 'D':
            return 19;
        case 'U':
            return 35;
        case 'P':
            return 44;
        case 'Q':
            return 40;
        case 'B':
            return 19;
        case 'I':
            return 50;
        case 'N':
            return 20;
        case 'O':
            return 48;
        default:
            return 0;
    }
}

namespace detail {

constexpr std::size_t kBody = 11;  // type, locate, tracking, 6-byte timestamp

inline Header read_header(const std::uint8_t* p) {
    return {static_cast<char>(p[0]), load_be16(p + 1), load_be16(p + 3), load_be48(p + 5)};
}

template <std::size_t N>
inline void copy_to(char (&dst)[N], const std::uint8_t* src) {
    std::memcpy(dst, src, N);
}

inline AddOrder read_add(const std::uint8_t* p, const Header& h) {
    const std::uint8_t* b = p + kBody;
    AddOrder m{h, load_be64(b), static_cast<char>(b[8]), load_be32(b + 9), {}, load_be32(b + 21),
               {}};
    copy_to(m.stock, b + 13);
    if (h.type == 'F') copy_to(m.mpid, b + 25);
    return m;
}

inline StockDirectory read_directory(const std::uint8_t* p, const Header& h) {
    const std::uint8_t* b = p + kBody;
    StockDirectory m{};
    m.h = h;
    copy_to(m.stock, b);
    m.market_category = static_cast<char>(b[8]);
    m.financial_status = static_cast<char>(b[9]);
    m.round_lot_size = load_be32(b + 10);
    m.round_lots_only = static_cast<char>(b[14]);
    m.issue_classification = static_cast<char>(b[15]);
    copy_to(m.issue_subtype, b + 16);
    m.authenticity = static_cast<char>(b[18]);
    m.short_sale_threshold = static_cast<char>(b[19]);
    m.ipo_flag = static_cast<char>(b[20]);
    m.luld_tier = static_cast<char>(b[21]);
    m.etp_flag = static_cast<char>(b[22]);
    m.etp_leverage = load_be32(b + 23);
    m.inverse = static_cast<char>(b[27]);
    return m;
}

}  // namespace detail

// Decodes one message of exactly `n` bytes and calls `h` with the decoded struct.
// Types this decoder does not unpack (Y, L, V, W, K, J, h, N, O) call `h(const Header&)`.
// Never reads past `p + n`; any length or type mismatch is reported, not decoded.
template <class Handler>
Status dispatch(const std::uint8_t* p, std::size_t n, Handler& h) {
    using namespace detail;
    if (n == 0) return Status::Truncated;
    const std::size_t want = length_of(static_cast<char>(p[0]));
    if (want == 0) return Status::UnknownType;
    if (n != want) return n < want ? Status::Truncated : Status::BadLength;

    const Header hd = read_header(p);
    const std::uint8_t* b = p + kBody;
    switch (hd.type) {
        case 'S':
            h(SystemEvent{hd, static_cast<char>(b[0])});
            break;
        case 'R':
            h(read_directory(p, hd));
            break;
        case 'H': {
            TradingAction m{hd, {}, static_cast<char>(b[8]), static_cast<char>(b[9]), {}};
            copy_to(m.stock, b);
            copy_to(m.reason, b + 10);
            h(m);
            break;
        }
        case 'A':
        case 'F':
            h(read_add(p, hd));
            break;
        case 'E':
            h(OrderExecuted{hd, load_be64(b), load_be32(b + 8), load_be64(b + 12)});
            break;
        case 'C':
            h(OrderExecutedPrice{hd, load_be64(b), load_be32(b + 8), load_be64(b + 12),
                                 static_cast<char>(b[20]), load_be32(b + 21)});
            break;
        case 'X':
            h(OrderCancel{hd, load_be64(b), load_be32(b + 8)});
            break;
        case 'D':
            h(OrderDelete{hd, load_be64(b)});
            break;
        case 'U':
            h(OrderReplace{hd, load_be64(b), load_be64(b + 8), load_be32(b + 16),
                           load_be32(b + 20)});
            break;
        case 'P': {
            Trade m{hd, load_be64(b),      static_cast<char>(b[8]), load_be32(b + 9),
                    {}, load_be32(b + 21), load_be64(b + 25)};
            copy_to(m.stock, b + 13);
            h(m);
            break;
        }
        case 'Q': {
            CrossTrade m{hd,
                         load_be64(b),
                         {},
                         load_be32(b + 16),
                         load_be64(b + 20),
                         static_cast<char>(b[28])};
            copy_to(m.stock, b + 8);
            h(m);
            break;
        }
        case 'B':
            h(BrokenTrade{hd, load_be64(b)});
            break;
        case 'I': {
            Imbalance m{hd,
                        load_be64(b),
                        load_be64(b + 8),
                        static_cast<char>(b[16]),
                        {},
                        load_be32(b + 25),
                        load_be32(b + 29),
                        load_be32(b + 33),
                        static_cast<char>(b[37]),
                        static_cast<char>(b[38])};
            copy_to(m.stock, b + 17);
            h(m);
            break;
        }
        default:
            h(hd);
            break;
    }
    return Status::Ok;
}

// BinaryFILE framing: each message is preceded by a 2-byte big-endian length.
struct Frame {
    const std::uint8_t* data;
    std::size_t size;
};

// Returns bytes consumed and fills `out`, or 0 when `avail` holds no complete frame.
inline std::size_t next_frame(const std::uint8_t* p, std::size_t avail, Frame& out) {
    if (avail < 2) return 0;
    const std::size_t len = load_be16(p);
    if (avail < 2 + len) return 0;
    out = {p + 2, len};
    return 2 + len;
}

}  // namespace hft::itch
