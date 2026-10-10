#pragma once

// Units and messages shared by the order manager, risk, ledger and simulated venue.
// Prices are 1e-4 USD per share, sizes 1e-6 share, money 1e-10 USD, so a price times a size
// is an exact amount of money and nothing in the ledger rounds.

#include <cstdint>
#include <type_traits>

#include "pm/gamma.hpp"

namespace hft::exec {

using Px = std::int32_t;   // 1e-4 USD per share, 0..kPxOne
using Qty = std::int64_t;  // 1e-6 share
using Usd = std::int64_t;  // 1e-10 USD; range +-9.2e8 USD
using Ns = std::int64_t;   // receive clock

inline constexpr Px kPxOne = 10'000;
inline constexpr Qty kShare = 1'000'000;
inline constexpr Usd kDollar = 10'000'000'000;

constexpr Usd notional(Px p, Qty q) { return Usd{p} * q; }

// Taker fee per the market's schedule: qty * rate * (p (1 - p))^exp USD, rounded half up to
// 1e-5 USD as the venue does (VENUE F17). Exact for exp <= 2, the parser's limit.
inline Usd taker_fee(Qty q, Px p, const pm::MarketRules& r) {
    if (!r.fees || q <= 0) return 0;
    const __int128 pp = static_cast<__int128>(p) * (kPxOne - p);  // (p (1 - p)) in 1e-8
    __int128 num = static_cast<__int128>(q) * r.fee_rate_ppm;     // 1e-12 USD per unit of (p (1 - p))^exp
    __int128 den = 100;                                           // to 1e-10 USD: 1e-12 / 1e-10
    for (int e = 0; e < r.fee_exp; ++e) num *= pp, den *= 100'000'000;
    constexpr __int128 step = 100'000;  // 1e-5 USD in 1e-10 units
    const __int128 steps = (2 * num + den * step) / (2 * den * step);
    return static_cast<Usd>(steps * step);
}

enum class Side : std::uint8_t { Buy, Sell };
enum class Tif : std::uint8_t { Gtc, Gtd, Fok, Fak };

struct OrderIntent {
    std::uint32_t token;
    Side side;
    Tif tif;
    bool post_only;
    std::uint16_t strategy;
    Px px;
    Qty qty;
    std::uint32_t tag;  // strategy's own reference
};

struct VenueReq {
    enum Kind : std::uint8_t { New, Cancel, Status } kind;  // Status: resend the order's fills, then its state
    Side side;
    Tif tif;
    bool post_only;
    std::uint32_t token;
    Px px;
    Qty qty;
    std::uint64_t cl_id;
};

struct VenueRpt {
    enum Kind : std::uint8_t { Ack, Reject, Fill, CancelAck, CancelReject, Expired, Status, Settled, SettleFailed } kind;
    // On Ack: Live, Matched, Delayed, Unmatched (VENUE F9, F10). On Status: Live, Filled, Cancelled, NotFound.
    enum Venue : std::uint8_t { None, Live, Matched, Delayed, Unmatched, Filled, Cancelled, NotFound } status;
    std::uint16_t reason;
    Px px;
    Qty qty;
    Usd fee;
    std::uint64_t cl_id, venue_id, fill_id;
    Ns venue_ns;
};

static_assert(std::is_trivially_copyable_v<OrderIntent> && sizeof(OrderIntent) == 32);
static_assert(std::is_trivially_copyable_v<VenueReq> && sizeof(VenueReq) == 32);
static_assert(std::is_trivially_copyable_v<VenueRpt> && sizeof(VenueRpt) == 56);

// Client order ids: session number in the top 16 bits, a sequence below. A session number is
// used once (DESIGN T6), so ids never repeat across runs.
inline constexpr std::uint32_t session_of(std::uint64_t id) { return static_cast<std::uint32_t>(id >> 48); }
inline constexpr std::uint64_t seq_of(std::uint64_t id) { return id & ((1ull << 48) - 1); }

class ClientIds {
   public:
    explicit ClientIds(std::uint16_t session) : base_(std::uint64_t{session} << 48) {}
    std::uint64_t next() { return base_ | ++seq_; }

   private:
    std::uint64_t base_, seq_ = 0;
};

}  // namespace hft::exec
