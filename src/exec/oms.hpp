#pragma once

// Order manager: one record per order, an explicit state machine (DESIGN S1-S18), reservations
// in the ledger and the risk exposure, timeouts that freeze a token until the venue confirms
// its orders, and reconciliation by asking the venue to resend an order's fills. Index-addressed
// and allocation-free once its pools have grown.
//
// Rules that keep it honest:
// - a fill is applied by quantity and at most once (fill ids remembered per order);
// - cumulative fill only grows and never passes the order size;
// - any report the machine cannot explain is counted and trips the kill switch.

#include <cstdint>
#include <utility>
#include <vector>

#include "book/id_map.hpp"
#include "core/pool.hpp"
#include "exec/risk.hpp"

namespace hft::exec {

enum class OrdState : std::uint8_t { PendingNew, Live, Partial, PendingCancel, Filled, Cancelled, Rejected, Expired, Unknown };
constexpr bool terminal(OrdState s) {
    return s == OrdState::Filled || s == OrdState::Cancelled || s == OrdState::Rejected || s == OrdState::Expired;
}

// What the listener is told about an order.
enum class OrdEvent : std::uint8_t { Acked, Rejected, Fill, Cancelled, Expired, Unknown, Resolved };

struct Order {
    std::uint64_t cl_id = 0, venue_id = 0;
    OrderIntent in{};
    OrdState state = OrdState::PendingNew;
    Qty cum = 0;
    Usd cash_left = 0, fee_left = 0;  // buy reservations not yet released
    Qty pos_left = 0;                 // sell reservation not yet released
    Ns deadline = 0;                  // ack or cancel timeout; 0 = none
    Ns done_ns = 0;                   // when it became terminal
    std::uint32_t fills = 0xffffffffu;  // head of this order's fill-id list (Oms::fill_ids_)
    std::uint32_t nfills = 0;
};

struct OmsConfig {
    Ns ack_timeout = 5'000'000'000;
    Ns cancel_timeout = 5'000'000'000;
    Ns keep_done = 60'000'000'000;  // finished orders stay this long, to recognise late duplicates
};

class Oms {
   public:
    Oms(std::uint16_t session, Risk& risk, Exposure& exp, Ledger& ledger, OmsConfig cfg = {})
        : ids_(session), risk_(risk), exp_(exp), ledger_(ledger), cfg_(cfg), orders_(256), index_(256), fill_ids_(256) {}

    void ensure(std::uint32_t tokens) {
        if (tokens > unknown_.size()) unknown_.resize(tokens, 0);
    }

    // Risk check, reservation and the New request. Returns the client id, or 0 with `why` set.
    template <class Sink>
    std::uint64_t submit(const OrderIntent& in, const MarketView& mv, Ns now, Sink&& sink, Reject* why = nullptr) {
        MarketView m = mv;
        m.frozen = m.frozen || unknown_[in.token] > 0;
        const Reject r = risk_.check(in, m, exp_, ledger_, now);
        if (why) *why = r;
        if (r != Reject::None) return 0;
        const std::uint32_t i = alloc();
        Order& o = orders_[i];
        o = Order{};
        o.cl_id = ids_.next();
        o.in = in;
        o.deadline = now + cfg_.ack_timeout;
        if (in.side == Side::Buy) {
            o.cash_left = notional(in.px, in.qty);
            o.fee_left = mv.rules ? taker_fee(in.qty, in.px, *mv.rules) : 0;
            ledger_.reserve_cash(o.cash_left + o.fee_left);
        } else {
            o.pos_left = in.qty;
            ledger_.reserve_pos(in.token, in.qty);
        }
        exp_.on_open(in);
        index_.insert(seq_of(o.cl_id), i);
        ++open_;
        sink(VenueReq{VenueReq::New, in.side, in.tif, in.post_only, in.token, in.px, in.qty, o.cl_id});
        return o.cl_id;
    }

    // Cancel request; false when the order is unknown, finished, already cancelling, or throttled.
    template <class Sink>
    bool cancel(std::uint64_t cl, Ns now, Sink&& sink) {
        Order* o = find(cl);
        if (!o || terminal(o->state) || o->state == OrdState::PendingCancel || o->state == OrdState::Unknown) return false;
        if (!risk_.check_cancel(now)) return false;
        o->state = OrdState::PendingCancel;
        o->deadline = now + cfg_.cancel_timeout;
        sink(VenueReq{VenueReq::Cancel, o->in.side, o->in.tif, false, o->in.token, o->in.px, o->in.qty, cl});
        return true;
    }

    // Kill switch path: cancel every open order. Cancels bypass the rate limit here, on purpose.
    template <class Sink>
    void cancel_all(Ns now, Sink&& sink) {
        for (std::uint32_t i = 0; i < used_; ++i) {
            Order& o = orders_[i];
            if (!o.cl_id || terminal(o.state) || o.state == OrdState::PendingCancel) continue;
            if (o.state != OrdState::Unknown) o.state = OrdState::PendingCancel, o.deadline = now + cfg_.cancel_timeout;
            sink(VenueReq{VenueReq::Cancel, o.in.side, o.in.tif, false, o.in.token, o.in.px, o.in.qty, o.cl_id});
        }
    }

    // Applies a venue report. The listener gets (order, event, fill qty, fill px).
    template <class Listener>
    void on_report(const VenueRpt& r, Ns now, Listener&& on) {
        if (r.kind == VenueRpt::Settled || r.kind == VenueRpt::SettleFailed) {
            const bool ok = r.kind == VenueRpt::Settled ? ledger_.settled(r.fill_id) : ledger_.failed(r.fill_id);
            if (!ok) return void(++orphan_settlements_);  // duplicate, or a fill report we never got
            if (r.kind == VenueRpt::SettleFailed) ++settle_failed_;
            return;
        }
        Order* o = find(r.cl_id);
        if (!o) return illegal(now, r.kind, 255);
        const OrdState s = o->state;
        last_kind_ = r.kind, last_state_ = static_cast<std::uint8_t>(s), last_reason_ = r.reason;
        switch (r.kind) {
            case VenueRpt::Ack:
                if (s == OrdState::Rejected) return illegal(now);
                o->venue_id = r.venue_id;
                if (s == OrdState::PendingNew) o->state = OrdState::Live, o->deadline = 0, on(*o, OrdEvent::Acked, 0, 0);
                else if (s == OrdState::Unknown) resolve(*o, o->cum ? OrdState::Partial : OrdState::Live, on);
                return;
            case VenueRpt::Reject:
                if (s == OrdState::Rejected) return void(++duplicates_);
                // A cancel can go out before the ack; the order may then still be rejected.
                if (s != OrdState::PendingNew && s != OrdState::Unknown && !(s == OrdState::PendingCancel && !o->venue_id))
                    return illegal(now);
                if (s == OrdState::Unknown) unfreeze(*o);
                finish(*o, OrdState::Rejected, now);
                on(*o, OrdEvent::Rejected, 0, 0);
                return;
            case VenueRpt::Fill: {
                if (seen(*o, r.fill_id)) return void(++duplicates_);
                if (terminal(s) || r.qty <= 0 || o->cum + r.qty > o->in.qty ||
                    (o->in.side == Side::Buy ? r.px > o->in.px : r.px < o->in.px))
                    return illegal(now);
                remember(*o, r.fill_id);
                apply_fill(*o, r);
                if (o->cum == o->in.qty) {
                    if (s == OrdState::Unknown) unfreeze(*o);
                    finish(*o, OrdState::Filled, now);
                } else if (s == OrdState::PendingNew || s == OrdState::Live) {
                    o->state = OrdState::Partial, o->deadline = 0;
                }
                on(*o, OrdEvent::Fill, r.qty, r.px);
                return;
            }
            case VenueRpt::CancelAck:
            case VenueRpt::Expired:
                // The same final report again (at-least-once delivery) changes nothing.
                if ((r.kind == VenueRpt::CancelAck && s == OrdState::Cancelled) || (r.kind == VenueRpt::Expired && s == OrdState::Expired))
                    return void(++duplicates_);
                if (terminal(s)) return illegal(now);
                if (s == OrdState::Unknown) unfreeze(*o);
                finish(*o, r.kind == VenueRpt::CancelAck ? OrdState::Cancelled : OrdState::Expired, now);
                on(*o, r.kind == VenueRpt::CancelAck ? OrdEvent::Cancelled : OrdEvent::Expired, 0, 0);
                return;
            case VenueRpt::CancelReject:  // the order traded or finished first
                if (s == OrdState::PendingCancel) o->state = o->cum ? OrdState::Partial : OrdState::Live, o->deadline = 0;
                else if (s == OrdState::Unknown) resolve(*o, o->cum ? OrdState::Partial : OrdState::Live, on);
                return;
            case VenueRpt::Status:  // after the resent fills: the venue's view of the order
                if (r.qty != o->cum) {  // fills the venue has and we do not, or the reverse
                    ++mismatches_;
                    risk_.kill(KillReason::Mismatch, now);
                }
                if (s != OrdState::Unknown) return;
                switch (r.status) {
                    case VenueRpt::Live: resolve(*o, o->cum ? OrdState::Partial : OrdState::Live, on); return;
                    case VenueRpt::Cancelled: unfreeze(*o), finish(*o, OrdState::Cancelled, now), on(*o, OrdEvent::Cancelled, 0, 0); return;
                    case VenueRpt::NotFound: unfreeze(*o), finish(*o, OrdState::Rejected, now), on(*o, OrdEvent::Rejected, 0, 0); return;
                    default: return;  // Filled arrives through the resent fills
                }
            default: return illegal(now);
        }
    }

    // Timeouts: an order with no ack or no cancel answer in time becomes Unknown, its token is
    // frozen, and the venue is asked for its status. Finished orders past keep_done are freed.
    template <class Sink>
    void on_timer(Ns now, Sink&& sink) {
        for (std::uint32_t i = 0; i < used_; ++i) {
            Order& o = orders_[i];
            if (!o.cl_id) continue;
            if (terminal(o.state)) {
                if (now - o.done_ns >= cfg_.keep_done) release(i);
                continue;
            }
            if (o.deadline && now >= o.deadline) {
                if (o.state != OrdState::Unknown) ++unknown_[o.in.token], ++timeouts_;
                o.state = OrdState::Unknown;
                o.deadline = now + cfg_.ack_timeout;  // ask again if the answer is lost too
                sink(VenueReq{VenueReq::Status, o.in.side, o.in.tif, false, o.in.token, o.in.px, o.in.qty, o.cl_id});
            }
        }
    }

    const Order* order(std::uint64_t cl) const {
        const std::uint32_t i = index_.find(seq_of(cl));
        return i == book::kNoOrder ? nullptr : &orders_[i];
    }
    bool frozen(std::uint32_t t) const { return unknown_[t] > 0; }
    std::uint32_t open_orders() const { return open_; }
    std::uint64_t illegal_count() const { return illegal_; }
    std::uint64_t duplicates() const { return duplicates_; }
    std::uint64_t timeouts() const { return timeouts_; }
    std::uint64_t mismatches() const { return mismatches_; }
    std::uint64_t settle_failures() const { return settle_failed_; }
    std::uint64_t orphan_settlements() const { return orphan_settlements_; }
    // The first illegal report: its kind and the order state it met (255: unknown order).
    std::pair<std::uint8_t, std::uint8_t> first_illegal() const { return first_illegal_; }
    std::uint16_t first_illegal_reason() const { return first_reason_; }

   private:
    Order* find(std::uint64_t cl) {
        const std::uint32_t i = index_.find(seq_of(cl));
        return i == book::kNoOrder || orders_[i].cl_id != cl ? nullptr : &orders_[i];
    }
    std::uint32_t alloc() {
        if (free_ != book::kNoOrder) {
            const std::uint32_t i = free_;
            free_ = static_cast<std::uint32_t>(orders_[i].venue_id);  // free list threads through venue_id
            return i;
        }
        orders_.reserve(used_ + 1);
        return used_++;
    }
    void release(std::uint32_t i) {
        for (std::uint32_t f = orders_[i].fills; f != book::kNoOrder;) {  // return the fill ids
            const std::uint32_t next = fill_ids_[f].next;
            fill_ids_[f].next = free_fill_, free_fill_ = f, f = next;
        }
        index_.erase(seq_of(orders_[i].cl_id));
        orders_[i].cl_id = 0;
        orders_[i].venue_id = free_;
        free_ = i;
    }

    // Every fill id an order has had, as a list in a shared pool: a large resting order can be
    // hit by many takers, and a duplicate of any earlier fill must still be caught.
    bool seen(const Order& o, std::uint64_t id) const {
        for (std::uint32_t f = o.fills; f != book::kNoOrder; f = fill_ids_[f].next)
            if (fill_ids_[f].id == id) return true;
        return false;
    }
    void remember(Order& o, std::uint64_t id) {
        std::uint32_t f = free_fill_;
        if (f != book::kNoOrder) free_fill_ = fill_ids_[f].next;
        else fill_ids_.reserve(used_fill_ + 1), f = used_fill_++;
        fill_ids_[f] = {id, o.fills};
        o.fills = f;
        ++o.nfills;
    }

    void apply_fill(Order& o, const VenueRpt& r) {
        const Qty before = o.in.qty - o.cum;
        if (o.in.side == Side::Buy) {
            const Usd cash = notional(o.in.px, r.qty);
            const Usd fee = static_cast<Usd>(static_cast<__int128>(o.fee_left) * r.qty / before);
            ledger_.release_cash(cash + fee);
            o.cash_left -= cash, o.fee_left -= fee;
        } else {
            ledger_.release_pos(o.in.token, r.qty);
            o.pos_left -= r.qty;
        }
        exp_.on_reduce(o.in, r.qty);
        o.cum += r.qty;
        ledger_.fill(r.fill_id, o.in.token, o.in.side, r.px, r.qty, r.fee);
    }

    void finish(Order& o, OrdState s, Ns now) {
        if (o.in.side == Side::Buy) ledger_.release_cash(o.cash_left + o.fee_left), o.cash_left = o.fee_left = 0;
        else ledger_.release_pos(o.in.token, o.pos_left), o.pos_left = 0;
        exp_.on_close(o.in, o.in.qty - o.cum);
        o.state = s, o.deadline = 0, o.done_ns = now;
        --open_;
    }
    template <class Listener>
    void resolve(Order& o, OrdState s, Listener& on) {
        unfreeze(o);
        o.state = s, o.deadline = 0;
        on(o, OrdEvent::Resolved, 0, 0);
    }
    void unfreeze(Order& o) { --unknown_[o.in.token]; }
    void illegal(Ns now) { illegal(now, last_kind_, last_state_); }
    void illegal(Ns now, std::uint8_t kind, std::uint8_t state) {
        if (!illegal_++) first_illegal_ = {kind, state}, first_reason_ = last_reason_;
        risk_.kill(KillReason::Internal, now);
    }

    ClientIds ids_;
    Risk& risk_;
    Exposure& exp_;
    Ledger& ledger_;
    OmsConfig cfg_;
    Pool<Order> orders_;
    book::LinearMap index_;
    struct FillId {
        std::uint64_t id;
        std::uint32_t next;
    };
    Pool<FillId> fill_ids_;
    std::uint32_t used_fill_ = 0, free_fill_ = book::kNoOrder;
    std::vector<std::uint32_t> unknown_;  // Unknown orders per token
    std::uint32_t used_ = 0, free_ = book::kNoOrder, open_ = 0;
    std::pair<std::uint8_t, std::uint8_t> first_illegal_{0, 0};
    std::uint8_t last_kind_ = 0, last_state_ = 0;
    std::uint16_t last_reason_ = 0, first_reason_ = 0;
    std::uint64_t illegal_ = 0, duplicates_ = 0, timeouts_ = 0, mismatches_ = 0, settle_failed_ = 0, orphan_settlements_ = 0;
};

}  // namespace hft::exec
