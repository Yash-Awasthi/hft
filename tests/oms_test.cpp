#include "exec/oms.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace hft::exec;
using S = OrdState;

namespace {

constexpr Usd kCap = 1'000'000 * kDollar;
constexpr Ns kSec = 1'000'000'000;

struct Rig {
    Ledger ledger{kCap};
    Exposure exp;
    Risk risk;
    Oms oms{3, risk, exp, ledger};
    hft::pm::MarketRules rules;
    MarketView mv;
    std::vector<VenueReq> sent;
    std::uint64_t fill_id = 1;

    Rig() {
        ledger.ensure(2), exp.ensure(2, 1), risk.ensure(2), oms.ensure(2);
        rules.tick = 10;
        mv = {4000, 4100, true, false, &rules};
    }
    auto sink() {
        return [this](const VenueReq& r) { sent.push_back(r); };
    }
    std::uint64_t buy(Qty q = 10 * kShare, Px px = 4000) {
        return oms.submit({0, Side::Buy, Tif::Gtc, false, 0, px, q, 0}, mv, kSec, sink());
    }
    void report(VenueRpt r) { oms.on_report(r, kSec, [](const Order&, OrdEvent, Qty, Px) {}); }
    void ack(std::uint64_t cl) { report({VenueRpt::Ack, VenueRpt::Live, 0, 0, 0, 0, cl, 77, 0, 0}); }
    void fill(std::uint64_t cl, Qty q, Px px = 4000) { report({VenueRpt::Fill, VenueRpt::None, 0, px, q, 0, cl, 77, fill_id++, 0}); }
    void plain(std::uint64_t cl, VenueRpt::Kind k) { report({k, VenueRpt::None, 0, 0, 0, 0, cl, 77, 0, 0}); }
    void status(std::uint64_t cl, VenueRpt::Venue v) {
        report({VenueRpt::Status, v, 0, 0, oms.order(cl)->cum, 0, cl, 77, 0, 0});
    }
};

enum Ev { Ack, Reject, FillPart, FillAll, CancelAck, CancelReject, Expired, StatusLive, StatusCancelled, StatusNotFound, kEv };
const char* kEvName[] = {"Ack", "Reject", "FillPart", "FillAll", "CancelAck", "CancelReject", "Expired", "StatusLive", "StatusCancelled", "StatusNotFound"};
const char* kStName[] = {"PendingNew", "Live", "Partial", "PendingCancel", "Filled", "Cancelled", "Rejected", "Expired", "Unknown"};

// Puts a fresh order into state s.
std::uint64_t reach(Rig& g, S s) {
    const std::uint64_t cl = g.buy();
    switch (s) {
        case S::PendingNew: break;
        case S::Live: g.ack(cl); break;
        case S::Partial: g.ack(cl), g.fill(cl, 4 * kShare); break;
        case S::PendingCancel: g.ack(cl), g.oms.cancel(cl, kSec, g.sink()); break;
        case S::Filled: g.ack(cl), g.fill(cl, 10 * kShare); break;
        case S::Cancelled: g.ack(cl), g.oms.cancel(cl, kSec, g.sink()), g.plain(cl, VenueRpt::CancelAck); break;
        case S::Rejected: g.plain(cl, VenueRpt::Reject); break;
        case S::Expired: g.ack(cl), g.plain(cl, VenueRpt::Expired); break;
        case S::Unknown: g.oms.on_timer(kSec + 10 * kSec, g.sink()); break;
    }
    return cl;
}

void apply(Rig& g, std::uint64_t cl, Ev e) {
    const Qty left = 10 * kShare - g.oms.order(cl)->cum;
    switch (e) {
        case Ack: g.ack(cl); break;
        case Reject: g.plain(cl, VenueRpt::Reject); break;
        case FillPart: g.fill(cl, left > kShare ? kShare : left + kShare); break;  // over-fill when nothing is left
        case FillAll: g.fill(cl, left > 0 ? left : kShare); break;
        case CancelAck: g.plain(cl, VenueRpt::CancelAck); break;
        case CancelReject: g.plain(cl, VenueRpt::CancelReject); break;
        case Expired: g.plain(cl, VenueRpt::Expired); break;
        case StatusLive: g.status(cl, VenueRpt::Live); break;
        case StatusCancelled: g.status(cl, VenueRpt::Cancelled); break;
        case StatusNotFound: g.status(cl, VenueRpt::NotFound); break;
        default: break;
    }
}

constexpr int X = -1;  // illegal: counted, kill switch tripped, state unchanged
// Expected next state for [state][event]. Status rows other than Unknown only reconcile.
const int kNext[9][kEv] = {
    //            Ack                  Reject                FillPart            FillAll             CancelAck              CancelReject           Expired              StLive               StCancelled          StNotFound
    /*PendingN*/ {int(S::Live),        int(S::Rejected),     int(S::Partial),    int(S::Filled),     int(S::Cancelled),     int(S::PendingNew),    int(S::Expired),     int(S::PendingNew),  int(S::PendingNew),  int(S::PendingNew)},
    /*Live    */ {int(S::Live),        X,                    int(S::Partial),    int(S::Filled),     int(S::Cancelled),     int(S::Live),          int(S::Expired),     int(S::Live),        int(S::Live),        int(S::Live)},
    /*Partial */ {int(S::Partial),     X,                    int(S::Partial),    int(S::Filled),     int(S::Cancelled),     int(S::Partial),       int(S::Expired),     int(S::Partial),     int(S::Partial),     int(S::Partial)},
    /*PendCxl */ {int(S::PendingCancel), X,                  int(S::PendingCancel), int(S::Filled),  int(S::Cancelled),     int(S::Live),          int(S::Expired),     int(S::PendingCancel), int(S::PendingCancel), int(S::PendingCancel)},
    /*Filled  */ {int(S::Filled),      X,                    X,                  X,                  X,                     int(S::Filled),        X,                   int(S::Filled),      int(S::Filled),      int(S::Filled)},
    /*Cancelld*/ {int(S::Cancelled),   X,                    X,                  X,                  int(S::Cancelled),     int(S::Cancelled),     X,                   int(S::Cancelled),   int(S::Cancelled),   int(S::Cancelled)},
    /*Rejected*/ {X,                   int(S::Rejected),     X,                  X,                  X,                     int(S::Rejected),      X,                   int(S::Rejected),    int(S::Rejected),    int(S::Rejected)},
    /*Expired */ {int(S::Expired),     X,                    X,                  X,                  X,                     int(S::Expired),       int(S::Expired),                   int(S::Expired),     int(S::Expired),     int(S::Expired)},
    /*Unknown */ {int(S::Live),        int(S::Rejected),     int(S::Unknown),    int(S::Filled),     int(S::Cancelled),     int(S::Live),          int(S::Expired),     int(S::Live),        int(S::Cancelled),   int(S::Rejected)},
};

}  // namespace

TEST(Oms, EveryStateAndEventFollowsTheTable) {
    for (int s = 0; s < 9; ++s)
        for (int e = 0; e < kEv; ++e) {
            Rig g;
            const std::uint64_t cl = reach(g, static_cast<S>(s));
            ASSERT_EQ(static_cast<int>(g.oms.order(cl)->state), s) << kStName[s];
            ASSERT_EQ(g.oms.illegal_count(), 0u);
            apply(g, cl, static_cast<Ev>(e));
            const Order& o = *g.oms.order(cl);
            const std::string at = std::string(kStName[s]) + " + " + kEvName[e];
            if (kNext[s][e] == X) {
                EXPECT_EQ(g.oms.illegal_count(), 1u) << at;
                EXPECT_EQ(g.risk.kill_reason(), KillReason::Internal) << at;
                EXPECT_EQ(static_cast<int>(o.state), s) << at;
            } else {
                EXPECT_EQ(g.oms.illegal_count(), 0u) << at;
                EXPECT_FALSE(g.risk.killed()) << at;
                EXPECT_EQ(kStName[static_cast<int>(o.state)], std::string(kStName[kNext[s][e]])) << at;
            }
            // Invariants after every step.
            EXPECT_EQ(g.oms.frozen(0), o.state == S::Unknown) << at;
            EXPECT_LE(o.cum, o.in.qty) << at;
            EXPECT_TRUE(g.ledger.identity_holds()) << at;
            if (terminal(o.state)) {
                EXPECT_EQ(g.ledger.available_cash(), g.ledger.cash()) << at;  // nothing left reserved
                EXPECT_EQ(g.exp.open(0), 0u) << at;
                EXPECT_EQ(g.exp.buy_usd(0), 0) << at;
                EXPECT_EQ(g.oms.open_orders(), 0u) << at;
            }
        }
}

// Random interleavings of submits, acks, fills (including duplicates, over-fills and fills past
// the limit), cancels, cancel answers, expiries, status replies and timeouts, against a naive
// reference: an order map with the accept rules restated. After every step the order manager's
// fills, positions, reservations, exposure and frozen tokens must match the reference.
RC_GTEST_PROP(Oms, MatchesANaiveReferenceUnderRandomReports, ()) {
    struct RefOrder {
        Side side;
        Px px;
        Qty qty, cum = 0;
        bool done = false, unknown = false;
        std::vector<std::uint64_t> fills;
    };
    Rig g;
    std::map<std::uint64_t, RefOrder> ref;
    std::vector<std::uint64_t> ids;
    Qty ref_pos = 0;
    std::uint64_t next_fill = 1;
    Ns now = kSec;
    int timers = 0;
    const int n = *rc::gen::inRange(1, 200);
    for (int step = 0; step < n; ++step) {
        const int op = *rc::gen::inRange(0, 12);
        if (op <= 1 || ids.empty()) {
            const bool sell = op == 1 && g.ledger.available_pos(0) >= 5 * kShare;
            const Px px = *rc::gen::element<Px>(3990, 4000, 4010);
            const Qty q = *rc::gen::elementOf(std::vector<Qty>{5 * kShare, 10 * kShare, 20 * kShare});
            const Qty qq = sell ? std::min(q, g.ledger.available_pos(0) / 10'000 * 10'000) : q;
            const auto cl = g.oms.submit({0, sell ? Side::Sell : Side::Buy, Tif::Gtc, false, 0, px, qq, 0}, g.mv, now, g.sink());
            if (cl) ids.push_back(cl), ref[cl] = {sell ? Side::Sell : Side::Buy, px, qq, 0, false, false, {}};
            continue;
        }
        const std::uint64_t cl = *rc::gen::elementOf(ids);
        RefOrder& r = ref[cl];
        const auto on = [](const Order&, OrdEvent, Qty, Px) {};
        switch (op) {
            case 2: g.oms.on_report({VenueRpt::Ack, VenueRpt::Live, 0, 0, 0, 0, cl, 9, 0, 0}, now, on); r.unknown = false; break;
            case 3: case 4: {  // fill: new id, or a resent one
                const bool dup = op == 4 && !r.fills.empty();
                const std::uint64_t fid = dup ? *rc::gen::elementOf(r.fills) : next_fill++;
                const Qty q = *rc::gen::elementOf(std::vector<Qty>{kShare, 3 * kShare, 5 * kShare, 30 * kShare});
                const Px p = r.side == Side::Buy ? r.px - *rc::gen::element<Px>(0, 0, 10, -10) : r.px + *rc::gen::element<Px>(0, 0, 10, -10);
                const bool seen = std::find(r.fills.begin(), r.fills.end(), fid) != r.fills.end();
                const bool ok = !seen && !r.done && r.cum + q <= r.qty && (r.side == Side::Buy ? p <= r.px : p >= r.px);
                g.oms.on_report({VenueRpt::Fill, VenueRpt::None, 0, p, q, 0, cl, 9, fid, 0}, now, on);
                if (ok) {
                    r.fills.push_back(fid), r.cum += q;
                    ref_pos += r.side == Side::Buy ? q : -q;
                    if (r.cum == r.qty) r.done = true, r.unknown = false;
                }
                break;
            }
            case 5: if (g.oms.cancel(cl, now, g.sink())) {} break;
            case 6: case 7: {
                const auto k = op == 6 ? VenueRpt::CancelAck : VenueRpt::Expired;
                g.oms.on_report({k, VenueRpt::None, 0, 0, 0, 0, cl, 9, 0, 0}, now, on);
                if (!r.done) r.done = true, r.unknown = false;
                break;
            }
            case 8: g.oms.on_report({VenueRpt::CancelReject, VenueRpt::None, 0, 0, 0, 0, cl, 9, 0, 0}, now, on); r.unknown = false; break;
            case 9:
                if (!r.done) {
                    g.oms.on_report({VenueRpt::Status, VenueRpt::Live, 0, 0, r.cum, 0, cl, 9, 0, 0}, now, on);
                    r.unknown = false;
                }
                break;
            case 10:
                if (timers < 8) {
                    ++timers, now += 6 * kSec;
                    g.oms.on_timer(now, g.sink());
                    for (auto& [id, o] : ref)
                        if (!o.done) o.unknown = true;  // every open order's deadline has passed
                }
                break;
            default: break;
        }
        if (op == 2 && g.oms.order(cl) && g.oms.order(cl)->state == S::PendingCancel) r.unknown = false;
        // Compare.
        Usd ref_reserved = 0, ref_open_buy = 0;
        bool ref_frozen = false;
        std::uint32_t ref_open = 0;
        for (const auto& [id, o] : ref) {
            const Order* m = g.oms.order(id);
            RC_ASSERT(m != nullptr);
            RC_ASSERT(m->cum == o.cum);
            RC_ASSERT(terminal(m->state) == o.done);
            if (o.done) continue;
            ++ref_open;
            ref_frozen = ref_frozen || m->state == S::Unknown;
            if (o.side == Side::Buy) ref_reserved += m->cash_left + m->fee_left, ref_open_buy += notional(o.px, o.qty - o.cum);
        }
        RC_ASSERT(g.ledger.pos(0) == ref_pos);
        RC_ASSERT(g.ledger.cash() - g.ledger.available_cash() == ref_reserved);
        RC_ASSERT(g.exp.buy_usd(0) == ref_open_buy);
        RC_ASSERT(g.oms.open_orders() == ref_open);
        RC_ASSERT(g.exp.open(0) == ref_open);
        RC_ASSERT(g.oms.frozen(0) == ref_frozen);
        RC_ASSERT(g.ledger.identity_holds());
    }
}

TEST(Oms, RejectAfterAnEarlyCancelIsLegalButNotAfterAnAck) {
    Rig g;
    const std::uint64_t a = g.buy();
    g.oms.cancel(a, kSec, g.sink());  // before any ack
    g.plain(a, VenueRpt::Reject);
    EXPECT_EQ(g.oms.order(a)->state, S::Rejected);
    EXPECT_EQ(g.oms.illegal_count(), 0u);
    const std::uint64_t b = g.buy();
    g.ack(b);
    g.oms.cancel(b, kSec, g.sink());
    g.plain(b, VenueRpt::Reject);  // acked orders are not rejected later
    EXPECT_EQ(g.oms.illegal_count(), 1u);
}

TEST(Oms, SettlementForAFillWeNeverSawIsCountedNotIllegal) {
    Rig g;
    g.report({VenueRpt::Settled, VenueRpt::None, 0, 0, 0, 0, 1, 1, 424242, 0});
    EXPECT_EQ(g.oms.orphan_settlements(), 1u);
    EXPECT_EQ(g.oms.illegal_count(), 0u);
}

TEST(Oms, DuplicateFillsAreIgnored) {
    Rig g;
    const std::uint64_t cl = g.buy();
    g.ack(cl);
    const VenueRpt f{VenueRpt::Fill, VenueRpt::None, 0, 4000, 3 * kShare, 0, cl, 77, 500, 0};
    g.report(f), g.report(f);
    EXPECT_EQ(g.oms.order(cl)->cum, 3 * kShare);
    EXPECT_EQ(g.oms.duplicates(), 1u);
    EXPECT_EQ(g.ledger.pos(0), 3 * kShare);
}

TEST(Oms, ReservationsFollowFillsAndAreReleasedAtTheEnd) {
    Rig g;
    const Usd before = g.ledger.available_cash();
    const std::uint64_t cl = g.buy(10 * kShare, 4000);
    const Usd fee = taker_fee(10 * kShare, 4000, g.rules);
    EXPECT_EQ(g.ledger.available_cash(), before - notional(4000, 10 * kShare) - fee);
    g.ack(cl);
    g.fill(cl, 4 * kShare, 3900);  // price improvement
    EXPECT_EQ(g.ledger.pos(0), 4 * kShare);
    EXPECT_EQ(g.exp.buy_usd(0), notional(4000, 6 * kShare));
    g.oms.cancel(cl, kSec, g.sink());
    g.plain(cl, VenueRpt::CancelAck);
    EXPECT_EQ(g.ledger.available_cash(), g.ledger.cash());
    EXPECT_EQ(g.ledger.cash(), kCap - notional(3900, 4 * kShare));
}

TEST(Oms, AFillAboveTheLimitIsIllegal) {
    Rig g;
    const std::uint64_t cl = g.buy(10 * kShare, 4000);
    g.ack(cl);
    g.fill(cl, kShare, 4010);
    EXPECT_EQ(g.oms.illegal_count(), 1u);
    EXPECT_EQ(g.oms.order(cl)->cum, 0);
}

TEST(Oms, TimeoutFreezesTheTokenAndAsksForStatus) {
    Rig g;
    const std::uint64_t cl = g.buy();
    g.sent.clear();
    g.oms.on_timer(kSec + 4 * kSec, g.sink());
    EXPECT_TRUE(g.sent.empty());  // not yet
    g.oms.on_timer(kSec + 5 * kSec, g.sink());
    ASSERT_EQ(g.sent.size(), 1u);
    EXPECT_EQ(g.sent[0].kind, VenueReq::Status);
    EXPECT_TRUE(g.oms.frozen(0));
    hft::exec::Reject why;
    EXPECT_EQ(g.oms.submit({0, Side::Buy, Tif::Gtc, false, 0, 4000, 10 * kShare, 0}, g.mv, 7 * kSec, g.sink(), &why), 0u);
    EXPECT_EQ(why, hft::exec::Reject::Frozen);
    g.status(cl, VenueRpt::Live);
    EXPECT_FALSE(g.oms.frozen(0));
}

TEST(Oms, StatusThatDisagreesWithOurFillsTripsTheKillSwitch) {
    Rig g;
    const std::uint64_t cl = g.buy();
    g.ack(cl);
    g.report({VenueRpt::Status, VenueRpt::Live, 0, 0, 2 * kShare, 0, cl, 77, 0, 0});  // venue says 2 filled, we saw 0
    EXPECT_EQ(g.oms.mismatches(), 1u);
    EXPECT_EQ(g.risk.kill_reason(), KillReason::Mismatch);
}

TEST(Oms, UnknownClientIdIsIllegal) {
    Rig g;
    g.ack(12345);
    EXPECT_EQ(g.oms.illegal_count(), 1u);
}

TEST(Oms, FinishedOrdersAreFreedAfterTheKeepWindowAndSlotsReused) {
    Rig g;
    const std::uint64_t a = g.buy();
    g.plain(a, VenueRpt::Reject);
    g.oms.on_timer(kSec + 30 * kSec, g.sink());
    EXPECT_NE(g.oms.order(a), nullptr);
    g.oms.on_timer(kSec + 61 * kSec, g.sink());
    EXPECT_EQ(g.oms.order(a), nullptr);
    const std::uint64_t b = g.buy();
    EXPECT_NE(a, b);
    EXPECT_NE(g.oms.order(b), nullptr);
}

TEST(Oms, CancelAllAsksToCancelEveryOpenOrder) {
    Rig g;
    const std::uint64_t a = g.buy(), b = g.buy(10 * kShare, 3990);
    g.ack(a);
    g.sent.clear();
    g.oms.cancel_all(kSec, g.sink());
    ASSERT_EQ(g.sent.size(), 2u);
    EXPECT_EQ(g.oms.order(a)->state, S::PendingCancel);
    EXPECT_EQ(g.oms.order(b)->state, S::PendingCancel);
}
