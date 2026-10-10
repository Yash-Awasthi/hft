#pragma once

// Pre-trade checks on every order and cancel, post-trade checks on every risk tick, and the
// kill switch (DESIGN R1-R16). Checks run cheapest first and return the first failure. The kill
// switch has no reset: a killed session stays killed until the process restarts (D11).

#include <cstdint>
#include <vector>

#include "exec/ledger.hpp"
#include "exec/types.hpp"

namespace hft::exec {

enum class Reject : std::uint8_t {
    None, Killed, Frozen, Tick, Size, NoBook, Collar, SelfCross, Position, Group, Gross, Cash, Throttle, TooMany, kCount
};
enum class KillReason : std::uint8_t { None, Operator, Loss, Deficit, RejectSpike, Mismatch, LegExposure, Internal };

struct RiskLimits {
    Usd token_cap = 50'000 * kDollar;    // held cost + open buys, per token (D3)
    Usd group_cap = 100'000 * kDollar;   // per event group
    Usd gross_cap = 500'000 * kDollar;
    Usd daily_stop = 20'000 * kDollar;   // loss that trips the kill switch
    Qty max_order_qty = 100'000 * kShare;
    Px collar_ticks = 2;                 // how far through the opposite best an order may go
    std::uint32_t max_open_per_token = 8, max_open = 512;
    // Venue limits at 80% (VENUE F21): 5000 orders / 10 s burst, 48000 / 10 min sustained.
    std::int64_t order_burst = 4000, order_rate_per_s = 400;
    std::int64_t sustained_burst = 38'400, sustained_rate_per_s = 64;
    std::int64_t cancel_burst = 4000, cancel_rate_per_s = 400;
    std::uint32_t reject_spike = 20;     // more venue rejects than this within the window: kill
    Ns reject_window = 10'000'000'000;
    bool allow_short = false;  // only for comparing with models that assume shorting (D25)
};

// Book state the checks need for one token.
struct MarketView {
    Px best_bid = -1, best_ask = -1;  // -1: side empty
    bool fresh = false;               // book valid and recently updated
    bool frozen = false;              // unknown orders, closed market, tick change, stale feed
    const pm::MarketRules* rules = nullptr;
};

// Open orders per token, kept by the order manager: what is offered, what cash is committed,
// and our own best prices for the self-cross check. Group sums are kept alongside.
class Exposure {
   public:
    void ensure(std::uint32_t tokens, std::uint32_t groups) {
        if (tokens > toks_.size()) toks_.resize(tokens);
        if (groups > group_buy_.size()) group_buy_.resize(groups, 0);
        if (group_of_.size() < tokens) group_of_.resize(tokens, 0);
    }
    void set_group(std::uint32_t t, std::uint32_t g) { group_of_[t] = g; }

    void on_open(const OrderIntent& o) {
        Tok& k = toks_[o.token];
        ++k.open, ++open_;
        if (o.side == Side::Buy) {
            const Usd u = notional(o.px, o.qty);
            k.buy_usd += u, group_buy_[group_of_[o.token]] += u, buy_usd_ += u;
            if (o.px > k.best_bid) k.best_bid = o.px;
            ++k.buys;
        } else {
            if (k.best_ask < 0 || o.px < k.best_ask) k.best_ask = o.px;
            ++k.sells;
        }
    }
    // `remaining` of the order is no longer open (filled or cancelled); `done` when the order
    // itself has gone. Own best prices reset when a side has no open orders left; with several
    // orders on a side they stay at the most aggressive price seen, which only over-rejects.
    void on_reduce(const OrderIntent& o, Qty remaining) {
        if (o.side == Side::Buy) {
            const Usd u = notional(o.px, remaining);
            Tok& k = toks_[o.token];
            k.buy_usd -= u, group_buy_[group_of_[o.token]] -= u, buy_usd_ -= u;
        }
    }
    void on_close(const OrderIntent& o, Qty remaining) {
        on_reduce(o, remaining);
        Tok& k = toks_[o.token];
        --k.open, --open_;
        if (o.side == Side::Buy && --k.buys == 0) k.best_bid = -1;
        if (o.side == Side::Sell && --k.sells == 0) k.best_ask = -1;
    }

    Px own_bid(std::uint32_t t) const { return toks_[t].best_bid; }
    Px own_ask(std::uint32_t t) const { return toks_[t].best_ask; }
    Usd buy_usd(std::uint32_t t) const { return toks_[t].buy_usd; }
    Usd group_buy_usd(std::uint32_t g) const { return group_buy_[g]; }
    Usd buy_usd() const { return buy_usd_; }
    std::uint32_t open(std::uint32_t t) const { return toks_[t].open; }
    std::uint32_t open() const { return open_; }

   private:
    struct Tok {
        Px best_bid = -1, best_ask = -1;
        std::uint32_t open = 0, buys = 0, sells = 0;
        Usd buy_usd = 0;
    };
    std::vector<Tok> toks_;
    std::vector<Usd> group_buy_;
    std::vector<std::uint32_t> group_of_;
    Usd buy_usd_ = 0;
    std::uint32_t open_ = 0;
};

class Risk {
   public:
    explicit Risk(const RiskLimits& l = {})
        : l_(l),
          order_(l.order_burst, l.order_rate_per_s),
          sustained_(l.sustained_burst, l.sustained_rate_per_s),
          cancel_(l.cancel_burst, l.cancel_rate_per_s) {}

    void ensure(std::uint32_t tokens) {
        if (tokens > group_of_.size()) group_of_.resize(tokens, 0);
    }
    void set_group(std::uint32_t t, std::uint32_t g) {
        group_of_[t] = g;
    }

    Reject check(const OrderIntent& o, const MarketView& m, const Exposure& x, const Ledger& lg, Ns now) {
        const Reject r = evaluate(o, m, x, lg, now);
        ++counts_[static_cast<std::size_t>(r)];
        return r;
    }
    // Cancels pass every check but their rate; a kill never blocks a cancel.
    bool check_cancel(Ns now) { return cancel_.take(now); }

    // Every risk tick: loss on the worse of the two marks, settlement deficits.
    void on_tick(const Ledger& lg, const Px* mid_marks, const Px* liq_marks, Ns now) {
        const Usd a = lg.total(mid_marks), b = lg.total(liq_marks);
        if ((a < b ? a : b) < -l_.daily_stop) kill(KillReason::Loss, now);
        if (lg.deficits()) kill(KillReason::Deficit, now);
    }
    void on_venue_reject(Ns now) {
        rejects_.push_back(now);
        while (!rejects_.empty() && rejects_.front() <= now - l_.reject_window) rejects_.erase(rejects_.begin());
        if (rejects_.size() > l_.reject_spike) kill(KillReason::RejectSpike, now);
    }

    void kill(KillReason why, Ns now) {
        if (reason_ != KillReason::None) return;
        reason_ = why;
        kill_ns_ = now;
    }
    bool killed() const { return reason_ != KillReason::None; }
    KillReason kill_reason() const { return reason_; }
    Ns kill_ns() const { return kill_ns_; }
    std::uint64_t count(Reject r) const { return counts_[static_cast<std::size_t>(r)]; }

   private:
    // Integer token bucket: tokens in 1e-9 of a message, refilled at rate per second.
    class Bucket {
       public:
        Bucket(std::int64_t burst, std::int64_t rate) : cap_(burst * kUnit), tokens_(cap_), rate_(rate) {}
        bool take(Ns now) {
            if (have_ && now > last_) {
                const __int128 t = tokens_ + static_cast<__int128>(now - last_) * rate_;
                tokens_ = t > cap_ ? cap_ : static_cast<std::int64_t>(t);
            }
            if (!have_ || now > last_) last_ = now, have_ = true;
            if (tokens_ < kUnit) return false;
            tokens_ -= kUnit;
            return true;
        }
        bool has(Ns now) const {
            if (!have_ || now <= last_) return tokens_ >= kUnit;
            return tokens_ + static_cast<__int128>(now - last_) * rate_ >= kUnit;
        }

       private:
        static constexpr std::int64_t kUnit = 1'000'000'000;
        std::int64_t cap_, tokens_, rate_;
        Ns last_ = 0;
        bool have_ = false;
    };

    Reject evaluate(const OrderIntent& o, const MarketView& m, const Exposure& x, const Ledger& lg, Ns now) {
        const std::uint32_t t = o.token;
        if (killed()) return Reject::Killed;
        if (m.frozen) return Reject::Frozen;
        const Px tick = m.rules && m.rules->tick > 0 ? m.rules->tick : 1;
        if (o.px <= 0 || o.px >= kPxOne || o.px % tick != 0) return Reject::Tick;
        const Qty min_qty = m.rules ? m.rules->min_qty : 0;
        if (o.qty < min_qty || o.qty % (kShare / 100) != 0 || o.qty > l_.max_order_qty) return Reject::Size;
        if (!m.fresh || m.best_bid < 0 || m.best_ask < 0) return Reject::NoBook;
        const Px c = l_.collar_ticks * tick;
        if (o.side == Side::Buy ? o.px > m.best_ask + c : o.px < m.best_bid - c) return Reject::Collar;
        if (o.side == Side::Buy ? x.own_ask(t) >= 0 && o.px >= x.own_ask(t) : x.own_bid(t) >= 0 && o.px <= x.own_bid(t))
            return Reject::SelfCross;
        if (o.side == Side::Sell) {
            if (!l_.allow_short && o.qty > lg.available_pos(t)) return Reject::Position;
        } else {
            const Usd add = notional(o.px, o.qty);
            if (lg.cost(t) + x.buy_usd(t) + add > l_.token_cap) return Reject::Position;
            const std::uint32_t g = group_of_[t];
            if (x.group_buy_usd(g) + lg.group_cost(g) + add > l_.group_cap) return Reject::Group;
            const Usd gross = x.buy_usd() + lg.cost();
            if (gross + add > l_.gross_cap) return Reject::Gross;
            const Usd fee = m.rules ? taker_fee(o.qty, o.px, *m.rules) : 0;
            if (add + fee > lg.available_cash()) return Reject::Cash;
        }
        if (x.open(t) >= l_.max_open_per_token || x.open() >= l_.max_open) return Reject::TooMany;
        // Take from both buckets only when both have a token, so a refusal costs nothing.
        if (!order_.has(now) || !sustained_.has(now)) return Reject::Throttle;
        order_.take(now), sustained_.take(now);
        return Reject::None;
    }

    RiskLimits l_;
    Bucket order_, sustained_, cancel_;
    std::vector<std::uint32_t> group_of_;
    std::vector<Ns> rejects_;
    KillReason reason_ = KillReason::None;
    Ns kill_ns_ = 0;
    std::uint64_t counts_[static_cast<std::size_t>(Reject::kCount)] = {};
};

}  // namespace hft::exec
