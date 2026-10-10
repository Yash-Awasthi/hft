#pragma once

// Simulated venue for paper trading and replay (DESIGN V1-V30). It reads the same level-2 books
// the strategies see, matches our orders against them with a latency model, fills resting
// orders from recorded trades behind a queue-ahead count, settles fills later (some may fail),
// and can drop, duplicate or hold its reports. Every random decision is a Philox draw
// addressed by message number, so a seed reproduces a run exactly.
//
// Limits, stated with every result: level-2 data has no order ids (queue position is an
// estimate); other traders do not react to our orders; latency is a model.

#include <cstdint>
#include <vector>

#include "book/id_map.hpp"
#include "core/philox.hpp"
#include "core/pool.hpp"
#include "engine/scheduler.hpp"
#include "exec/types.hpp"
#include "pm/book.hpp"

namespace hft::exec {

struct SimConfig {
    Ns lat_in = 0, lat_out = 0;  // one-way order entry and report latency
    Ns jitter = 0;               // uniform [0, jitter) added to each one-way trip
    bool compat_side = false;    // D25: resting orders hit only on the side the feed names
    Ns settle_delay = 2'000'000'000;
    std::uint32_t p_settle_fail_ppm = 0, p_drop_ack_ppm = 0, p_drop_fill_ppm = 0, p_dup_ppm = 0;
    Ns disc_every = 0, disc_for = 0;  // a disconnect of disc_for every disc_every: reports held
    std::uint64_t seed = 1;
};

struct SimStats {
    std::uint64_t orders = 0, rejects = 0, taker_fills = 0, maker_fills = 0, expired = 0, cancels = 0;
    std::uint64_t dropped = 0, duplicated = 0, held = 0, settle_failed = 0;
    Qty taker_qty = 0, maker_qty = 0;
};

// BookOf: callable (std::uint32_t token) -> const pm::TokenBook&.
template <class BookOf>
class SimVenue {
   public:
    enum Reason : std::uint16_t { kNone, kTick, kSize, kClosed, kPostOnlyCross, kDuplicate, kNotFound, kFilled };

    SimVenue(const SimConfig& c, BookOf book)
        : c_(c), book_(book), rng_(c.seed, 0, 0), q_(4096), orders_(256), idx_(256), fills_(256), reqs_(64), rpts_(256) {}

    void ensure(std::uint32_t tokens) {
        if (tokens > toks_.size()) toks_.resize(tokens);
    }
    void set_rules(std::uint32_t t, const pm::MarketRules& r) { toks_[t].rules = r; }
    // The market closed or resolved: resting orders are cancelled, new ones rejected.
    void close(std::uint32_t t, Ns now) {
        toks_[t].closed = true;
        while (!toks_[t].resting.empty()) {
            VOrder& o = orders_[toks_[t].resting.back()];
            unrest(o);
            o.state = kDone;
            report(now, {VenueRpt::CancelAck, VenueRpt::None, kClosed, 0, 0, 0, o.cl, o.vid, 0, now});
        }
    }

    // A request leaves the client at `now`.
    void request(const VenueReq& r, Ns now) {
        const std::uint32_t i = alloc_req();
        reqs_[i] = {r, false};
        // One connection: requests arrive in the order they were sent, whatever the jitter.
        Ns at = now + c_.lat_in + jitter(++nreq_, 1);
        if (at < last_arrival_) at = last_arrival_;
        last_arrival_ = at;
        q_.push(static_cast<std::uint64_t>(at), engine::Kind::OrderArrival, i);
    }

    // Processes everything scheduled before `t` (or at `t` too when `inclusive`): arrivals and
    // delayed matches at the venue, and report deliveries to `out(const VenueRpt&)`.
    template <class Out>
    void run(Ns t, bool inclusive, Out&& out) {
        engine::Timed e;
        while (q_.peek(e) && (static_cast<Ns>(e.time) < t || (inclusive && static_cast<Ns>(e.time) == t))) {
            q_.pop(e);
            const Ns at = static_cast<Ns>(e.time);
            if (e.kind() == engine::Kind::Report) {
                const VenueRpt r = rpts_[e.payload];
                free_rpt(e.payload);
                out(r);
            } else {
                const Req rq = reqs_[e.payload];
                free_req(e.payload);
                arrive(rq, at);
            }
        }
    }

    // Book changes, after the caller applied them to the books.
    void on_snapshot(std::uint32_t t) {
        Tok& k = toks_[t];
        k.overlay.clear();
        for (const std::uint32_t i : k.resting) cap_ahead(orders_[i]);
    }
    void on_level(std::uint32_t t, bool buy, Px px) {
        Tok& k = toks_[t];
        for (std::size_t j = 0; j < k.overlay.size(); ++j)
            if (k.overlay[j].buy == buy && k.overlay[j].px == px) k.overlay[j] = k.overlay.back(), k.overlay.pop_back(), --j;
        for (const std::uint32_t i : k.resting) {
            VOrder& o = orders_[i];
            if ((o.side == Side::Buy) == buy && o.px == px) cap_ahead(o);
        }
    }
    // A trade printed at `px` for `size` shares; `taker_buy` is the feed's side (used only in
    // compat mode, D25). Resting orders fill at their own price, makers pay no fee (F17).
    void on_trade(std::uint32_t t, Px px, double size, bool taker_buy, Ns now) {
        Tok& k = toks_[t];
        Qty left = to_qty(size);  // shares at px still to be matched against our queue position
        for (std::size_t j = 0; j < k.resting.size(); ++j) {
            VOrder& o = orders_[k.resting[j]];
            const bool buy = o.side == Side::Buy;
            if (c_.compat_side && buy == taker_buy) continue;
            const bool through = buy ? px < o.px : px > o.px;
            if (!through && px != o.px) continue;
            Qty f = 0;
            if (through) {
                f = o.rem;
            } else if (left > o.ahead) {
                f = o.rem < left - o.ahead ? o.rem : left - o.ahead;
                left -= o.ahead + f;
                o.ahead = 0;
            } else {
                o.ahead -= left;
                left = 0;
            }
            if (f <= 0) continue;
            fill(o, o.px, f, 0, now);
            ++stats_.maker_fills, stats_.maker_qty += f;
            if (o.rem == 0) {
                unrest(o), o.state = kDone;
                --j;  // unrest moved another order into this slot
            }
        }
    }

    const SimStats& stats() const { return stats_; }
    // The venue's view of our position per token: every fill, less fills whose settlement it
    // decided will fail (the SettleFailed report reaches us settle_delay later).
    Qty position(std::uint32_t t) const { return toks_[t].pos; }
    std::size_t scheduled() const { return q_.size(); }

    // Venue-side invariants (V15-V17), for tests: every order's fills add up to its cumulative
    // quantity, never past its size, never at a price worse than its limit.
    bool check() const {
        for (std::uint32_t i = 0; i < used_; ++i) {
            const VOrder& o = orders_[i];
            if (!o.cl) continue;
            Qty sum = 0;
            for (std::uint32_t f = o.fills; f != book::kNoOrder; f = fills_[f].next) {
                const VFill& x = fills_[f];
                sum += x.qty;
                if (o.side == Side::Buy ? x.px > o.px : x.px < o.px) return false;
            }
            if (sum != o.cum || o.cum > o.qty || o.rem != o.qty - o.cum) return false;
        }
        return true;
    }

   private:
    enum State : std::uint8_t { kResting, kDelayed, kDone };
    struct VOrder {
        std::uint64_t cl = 0, vid = 0;
        std::uint32_t token = 0;
        Side side = Side::Buy;
        Tif tif = Tif::Gtc;
        bool post_only = false;
        State state = kResting;
        Px px = 0;
        Qty qty = 0, rem = 0, ahead = 0, cum = 0;
        std::uint32_t fills = book::kNoOrder;
    };
    struct VFill {
        std::uint64_t id;
        Px px;
        Qty qty;
        Usd fee;
        std::uint32_t next;
    };
    struct Req {
        VenueReq r;
        bool delayed;  // second visit of a delayed marketable order
    };
    struct Overlay {
        bool buy;
        Px px;
        Qty qty;
    };
    struct Tok {
        pm::MarketRules rules;
        bool closed = false;
        std::vector<std::uint32_t> resting;
        std::vector<Overlay> overlay;  // shares our taker orders took per level, until the level updates
        Qty pos = 0;
    };

    static Qty to_qty(double shares) { return static_cast<Qty>(shares * 1e6 + 0.5); }
    Ns jitter(std::uint32_t n, std::uint32_t purpose) const {
        return c_.jitter > 0 ? static_cast<Ns>(rng_.draw(n, purpose)[0] % static_cast<std::uint64_t>(c_.jitter)) : 0;
    }
    bool chance(std::uint32_t ppm, std::uint32_t n, std::uint32_t purpose) const {
        return ppm && rng_.draw(n, purpose)[1] % 1'000'000 < ppm;
    }

    // Reports leave the venue at `at` and arrive in order (a single connection): never before
    // the previous report, held to the end of a disconnect window if one is open. Settlement
    // comes from the chain on its own stream (`ordered` false) and does not hold the rest back.
    void report(Ns at, const VenueRpt& r, bool ordered = true) {
        const std::uint32_t n = ++nrpt_;
        if ((r.kind == VenueRpt::Ack && chance(c_.p_drop_ack_ppm, n, 3)) ||
            (r.kind == VenueRpt::Fill && chance(c_.p_drop_fill_ppm, n, 4)))
            return void(++stats_.dropped);
        Ns d = at + c_.lat_out + jitter(n, 2);
        if (c_.disc_every > 0 && d % c_.disc_every < c_.disc_for) d += c_.disc_for - d % c_.disc_every, ++stats_.held;
        if (ordered) {
            if (d < last_delivery_) d = last_delivery_;
            last_delivery_ = d;
        }
        const int copies = chance(c_.p_dup_ppm, n, 5) ? 2 : 1;
        stats_.duplicated += copies - 1;
        for (int k = 0; k < copies; ++k) {
            const std::uint32_t i = alloc_rpt();
            rpts_[i] = r;
            q_.push(static_cast<std::uint64_t>(d), engine::Kind::Report, i);
        }
    }

    void fill(VOrder& o, Px px, Qty q, Usd fee, Ns now) {
        const std::uint64_t id = ++fill_seq_;
        std::uint32_t f = free_fill_;
        if (f != book::kNoOrder) free_fill_ = fills_[f].next;
        else fills_.reserve(used_fill_ + 1), f = used_fill_++;
        fills_[f] = {id, px, q, fee, o.fills};
        o.fills = f;
        o.cum += q, o.rem -= q;
        toks_[o.token].pos += o.side == Side::Buy ? q : -q;
        report(now, {VenueRpt::Fill, VenueRpt::None, kNone, px, q, fee, o.cl, o.vid, id, now});
        const bool fails = chance(c_.p_settle_fail_ppm, static_cast<std::uint32_t>(id), 6);
        stats_.settle_failed += fails;
        if (fails) toks_[o.token].pos -= o.side == Side::Buy ? q : -q;
        report(now + c_.settle_delay, {fails ? VenueRpt::SettleFailed : VenueRpt::Settled, VenueRpt::None, kNone, 0, 0, 0, o.cl, o.vid, id, now}, false);
    }

    void arrive(const Req& rq, Ns at) {
        const VenueReq& r = rq.r;
        if (rq.delayed) return match_delayed(r.cl_id, at);
        const std::uint32_t found = idx_.find(seq_of(r.cl_id));
        VOrder* o = found == book::kNoOrder ? nullptr : &orders_[found];
        if (r.kind == VenueReq::Cancel) {
            if (o && o->state != kDone) {
                if (o->state == kResting) unrest(*o);
                o->state = kDone;
                ++stats_.cancels;
                return report(at, {VenueRpt::CancelAck, VenueRpt::None, kNone, 0, 0, 0, r.cl_id, o->vid, 0, at});
            }
            return report(at, {VenueRpt::CancelReject, VenueRpt::None, o ? kFilled : kNotFound, 0, 0, 0, r.cl_id, o ? o->vid : 0, 0, at});
        }
        if (r.kind == VenueReq::Status) {
            if (!o) return report(at, {VenueRpt::Status, VenueRpt::NotFound, kNotFound, 0, 0, 0, r.cl_id, 0, 0, at});
            for (std::uint32_t f = o->fills; f != book::kNoOrder; f = fills_[f].next) {
                const VFill& x = fills_[f];
                report(at, {VenueRpt::Fill, VenueRpt::None, kNone, x.px, x.qty, x.fee, o->cl, o->vid, x.id, at});
            }
            const auto st = o->state != kDone ? VenueRpt::Live : o->rem == 0 ? VenueRpt::Filled : VenueRpt::Cancelled;
            return report(at, {VenueRpt::Status, st, kNone, 0, o->cum, 0, o->cl, o->vid, 0, at});
        }
        if (o) return report(at, {VenueRpt::Reject, VenueRpt::None, kDuplicate, 0, 0, 0, r.cl_id, 0, 0, at});
        new_order(r, at);
    }

    void new_order(const VenueReq& r, Ns at) {
        ++stats_.orders;
        const std::uint32_t i = alloc_order();
        VOrder& o = orders_[i];
        o = VOrder{r.cl_id, ++vid_seq_, r.token, r.side, r.tif, r.post_only, kResting, r.px, r.qty, r.qty, 0, 0, book::kNoOrder};
        idx_.insert(seq_of(r.cl_id), i);
        const Tok& k = toks_[r.token];
        const Px tick = k.rules.tick > 0 ? k.rules.tick : 1;
        Reason why = kNone;
        if (k.closed) why = kClosed;
        else if (r.px <= 0 || r.px >= kPxOne || r.px % tick) why = kTick;
        else if (r.qty < k.rules.min_qty || r.qty % (kShare / 100)) why = kSize;
        else if (r.post_only && marketable(o)) why = kPostOnlyCross;
        if (why != kNone) {
            o.state = kDone;
            ++stats_.rejects;
            return report(at, {VenueRpt::Reject, VenueRpt::None, why, 0, 0, 0, o.cl, o.vid, 0, at});
        }
        if (marketable(o) && k.rules.delay_ms > 0) {  // F10: marketable orders wait, then match
            o.state = kDelayed;
            report(at, {VenueRpt::Ack, VenueRpt::Delayed, kNone, 0, 0, 0, o.cl, o.vid, 0, at});
            const std::uint32_t j = alloc_req();
            reqs_[j] = {r, true};
            q_.push(static_cast<std::uint64_t>(at + Ns{k.rules.delay_ms} * 1'000'000), engine::Kind::OrderArrival, j);
            return;
        }
        const bool takes = marketable(o);
        report(at, {VenueRpt::Ack, takes ? VenueRpt::Matched : VenueRpt::Live, kNone, 0, 0, 0, o.cl, o.vid, 0, at});
        execute(o, at);
    }

    void match_delayed(std::uint64_t cl, Ns at) {
        const std::uint32_t i = idx_.find(seq_of(cl));
        if (i == book::kNoOrder || orders_[i].state != kDelayed) return;  // cancelled meanwhile
        orders_[i].state = kResting;
        execute(orders_[i], at);
    }

    // Takes what the book offers within the limit, then rests or expires the remainder.
    void execute(VOrder& o, Ns at) {
        if (marketable(o)) {
            if (o.tif == Tif::Fok && available(o) < o.rem) return expire(o, at);
            take(o, at);
        }
        if (o.rem == 0) return void(o.state = kDone);
        if (o.tif == Tif::Fak || o.tif == Tif::Fok) return expire(o, at);
        rest(o);
    }
    void expire(VOrder& o, Ns at) {
        o.state = kDone;
        ++stats_.expired;
        report(at, {VenueRpt::Expired, VenueRpt::None, kNone, 0, 0, 0, o.cl, o.vid, 0, at});
    }

    bool marketable(const VOrder& o) const {
        const pm::TokenBook& b = book_(o.token);
        return o.side == Side::Buy ? b.best_ask() >= 0 && o.px >= b.best_ask() : b.best_bid() >= 0 && o.px <= b.best_bid();
    }
    Qty taken(std::uint32_t t, bool buy_side, Px px) const {
        for (const Overlay& v : toks_[t].overlay)
            if (v.buy == buy_side && v.px == px) return v.qty;
        return 0;
    }
    // Opposite-side levels within the limit, best first: fn(px, shares displayed minus taken).
    template <class Fn>
    void walk(const VOrder& o, Fn&& fn) const {
        const pm::TokenBook& b = book_(o.token);
        if (o.side == Side::Buy) {
            for (Px px = b.best_ask(); px >= 0 && px <= o.px; px = b.ask_at_or_above(px + 1))
                if (!fn(px, to_qty(b.size_at(false, px)) - taken(o.token, false, px))) return;
        } else {
            for (Px px = b.best_bid(); px >= 0 && px >= o.px; px = b.bid_at_or_below(px - 1))
                if (!fn(px, to_qty(b.size_at(true, px)) - taken(o.token, true, px))) return;
        }
    }
    Qty available(const VOrder& o) const {
        Qty n = 0;
        walk(o, [&](Px, Qty a) { return (n += a > 0 ? a : 0) < o.rem; });
        return n;
    }
    void take(VOrder& o, Ns at) {
        Tok& k = toks_[o.token];
        const bool level_side = o.side == Side::Sell;  // a buy takes asks (buy_side false)
        walk(o, [&](Px px, Qty a) {
            if (a <= 0) return true;
            const Qty q = a < o.rem ? a : o.rem;
            bool found = false;
            for (Overlay& v : k.overlay)
                if (v.buy == level_side && v.px == px) v.qty += q, found = true;
            if (!found) k.overlay.push_back({level_side, px, q});
            fill(o, px, q, taker_fee(q, px, k.rules), at);
            ++stats_.taker_fills, stats_.taker_qty += q;
            return o.rem > 0;
        });
    }

    void rest(VOrder& o) {
        const pm::TokenBook& b = book_(o.token);
        const bool buy = o.side == Side::Buy;
        const Px best = buy ? b.best_bid() : b.best_ask();
        const bool improves = best < 0 || (buy ? o.px > best : o.px < best);
        o.ahead = improves ? 0 : to_qty(b.size_at(buy, o.px));
        o.state = kResting;
        toks_[o.token].resting.push_back(static_cast<std::uint32_t>(&o - &orders_[0]));
    }
    void unrest(VOrder& o) {
        auto& v = toks_[o.token].resting;
        const auto me = static_cast<std::uint32_t>(&o - &orders_[0]);
        for (std::size_t j = 0; j < v.size(); ++j)
            if (v[j] == me) return void((v[j] = v.back(), v.pop_back()));
    }
    // A level cannot hold more ahead of us than it displays (cancels ahead are not credited).
    void cap_ahead(VOrder& o) {
        const Qty shown = to_qty(book_(o.token).size_at(o.side == Side::Buy, o.px));
        if (o.ahead > shown) o.ahead = shown;
    }

    std::uint32_t alloc_order() {
        orders_.reserve(used_ + 1);
        return used_++;
    }
    // ponytail: venue order records are kept for the whole session (Status may ask about any);
    // about 80 bytes per order, so a busy maker adds tens of MB per hour. Free old finished
    // orders if sessions get long.
    std::uint32_t alloc_req() {
        if (free_req_ != book::kNoOrder) {
            const std::uint32_t i = free_req_;
            free_req_ = static_cast<std::uint32_t>(reqs_[i].r.cl_id);
            return i;
        }
        reqs_.reserve(used_req_ + 1);
        return used_req_++;
    }
    void free_req(std::uint32_t i) { reqs_[i].r.cl_id = free_req_, free_req_ = i; }
    std::uint32_t alloc_rpt() {
        if (free_rpt_ != book::kNoOrder) {
            const std::uint32_t i = free_rpt_;
            free_rpt_ = static_cast<std::uint32_t>(rpts_[i].cl_id);
            return i;
        }
        rpts_.reserve(used_rpt_ + 1);
        return used_rpt_++;
    }
    void free_rpt(std::uint32_t i) { rpts_[i].cl_id = free_rpt_, free_rpt_ = i; }

    SimConfig c_;
    BookOf book_;
    rng::Stream rng_;
    engine::EventQueue q_;
    std::vector<Tok> toks_;
    Pool<VOrder> orders_;
    book::LinearMap idx_;
    Pool<VFill> fills_;
    Pool<Req> reqs_;
    Pool<VenueRpt> rpts_;
    std::uint32_t used_ = 0, used_fill_ = 0, free_fill_ = book::kNoOrder;
    std::uint32_t used_req_ = 0, free_req_ = book::kNoOrder, used_rpt_ = 0, free_rpt_ = book::kNoOrder;
    std::uint32_t nreq_ = 0, nrpt_ = 0;
    std::uint64_t vid_seq_ = 0, fill_seq_ = 0;
    Ns last_delivery_ = 0, last_arrival_ = 0;
    SimStats stats_;
};

}  // namespace hft::exec
