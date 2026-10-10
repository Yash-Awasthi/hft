// Arbitrary report sequences into the order manager: it must never crash, and fills, positions,
// reservations and exposure must stay consistent whatever the venue sends.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "exec/oms.hpp"

using namespace hft::exec;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* d, std::size_t n) {
    constexpr Ns kSec = 1'000'000'000;
    Ledger ledger(1'000'000 * kDollar);
    Exposure exp;
    Risk risk;
    Oms oms(1, risk, exp, ledger);
    ledger.ensure(2), exp.ensure(2, 1), risk.ensure(2), oms.ensure(2);
    hft::pm::MarketRules rules;
    rules.tick = 10;
    const MarketView mv{4000, 4100, true, false, &rules};
    std::vector<std::uint64_t> ids;
    const auto sink = [](const VenueReq&) {};
    const auto on = [](const Order&, OrdEvent, Qty, Px) {};
    Ns now = kSec;
    for (std::size_t i = 0; i + 4 <= n; i += 4) {
        const std::uint8_t op = d[i] % 12, a = d[i + 1], b = d[i + 2], c = d[i + 3];
        const std::uint32_t tok = a & 1;
        const std::uint64_t cl = ids.empty() ? (std::uint64_t{1} << 48 | b) : ids[b % ids.size()];
        const Qty q = static_cast<Qty>(1 + c % 40) * kShare / 2;
        const Px px = static_cast<Px>(3950 + (a % 20) * 10);
        switch (op) {
            case 0: case 1: {
                const Side s = op ? Side::Sell : Side::Buy;
                if (const auto id = oms.submit({tok, s, Tif::Gtc, false, 0, px, q, 0}, mv, now, sink)) ids.push_back(id);
                break;
            }
            case 2: oms.on_report({VenueRpt::Ack, VenueRpt::Live, 0, 0, 0, 0, cl, 1, 0, now}, now, on); break;
            case 3: oms.on_report({VenueRpt::Fill, VenueRpt::None, 0, px, q, 0, cl, 1, std::uint64_t{c} + 1, now}, now, on); break;
            case 4: oms.on_report({VenueRpt::Reject, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on); break;
            case 5: oms.cancel(cl, now, sink); break;
            case 6: oms.on_report({VenueRpt::CancelAck, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on); break;
            case 7: oms.on_report({VenueRpt::CancelReject, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on); break;
            case 8: oms.on_report({VenueRpt::Expired, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on); break;
            case 9: {
                const auto v = static_cast<VenueRpt::Venue>(c % 8);
                oms.on_report({VenueRpt::Status, v, 0, 0, q, 0, cl, 1, 0, now}, now, on);
                break;
            }
            case 10: now += static_cast<Ns>(b) * kSec / 4, oms.on_timer(now, sink); break;
            case 11: oms.on_report({c & 1 ? VenueRpt::Settled : VenueRpt::SettleFailed, VenueRpt::None, 0, 0, 0, 0, cl, 1, std::uint64_t{c} + 1, now}, now, on); break;
        }
        // Invariants.
        Usd reserved = 0, open_buy[2] = {0, 0};
        std::uint32_t open = 0, open_tok[2] = {0, 0};
        bool unknown[2] = {false, false};
        for (const std::uint64_t id : ids) {
            const Order* o = oms.order(id);
            if (!o) continue;  // freed after the keep window
            if (o->cum < 0 || o->cum > o->in.qty) std::abort();
            if (terminal(o->state)) continue;
            ++open, ++open_tok[o->in.token];
            unknown[o->in.token] |= o->state == OrdState::Unknown;
            if (o->in.side == Side::Buy) reserved += o->cash_left + o->fee_left, open_buy[o->in.token] += notional(o->in.px, o->in.qty - o->cum);
        }
        if (!ledger.identity_holds()) std::abort();
        if (ledger.cash() - ledger.available_cash() != reserved) std::abort();
        if (oms.open_orders() != open || exp.open() != open) std::abort();
        for (std::uint32_t t = 0; t < 2; ++t)
            if (exp.buy_usd(t) != open_buy[t] || exp.open(t) != open_tok[t] || oms.frozen(t) != unknown[t]) std::abort();
    }
    return 0;
}
