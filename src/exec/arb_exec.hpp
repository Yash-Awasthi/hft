#pragma once

// Arbitrage executor (DESIGN X): turns an open window on a group of outcome tokens, exactly one
// of which pays 1, into orders, and handles sets that end up incomplete. The decisions are pure
// functions of the books: which windows to take (X2), at what size (X4) and net edge (X3), and
// whether to complete or unwind an incomplete set (X7, R10). ArbExec runs attempts with them.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "exec/ledger.hpp"
#include "exec/types.hpp"
#include "pm/book.hpp"

namespace hft::exec {

struct ArbParams {
    bool on = false;
    bool sequential = false;            // X5 policy Q (thinnest leg FOK first), else P (all legs FAK)
    Ns min_age = 0;                     // X2: window open at least this long
    Ns fresh = 2'000'000'000;           // X2: every leg's book updated within this
    Px min_edge = 0;                    // X2: net edge per set, 1e-4 USD
    Px slip_ticks = 0, latency_ticks = 1;  // X3: allowances per leg, in that leg's ticks
    Qty max_set = 1'000 * kShare;
    Usd attempt_bound = 1'000 * kDollar;   // R10 (D3)
    Ns leg_timeout = 5'000'000'000;     // X6: legs not finished by then are cancelled
    Ns hold_limit = 60'000'000'000;     // R15: leg exposure above attempt_bound for longer trips the kill switch
    Ns merge_delay = 0;                 // L6: relayer latency for merge and split
};

enum class ArbFilter : std::uint8_t { Pass, Young, Stale, Frozen, SelfCross, Size, Edge, kCount };

// One leg as the executor sees it.
struct LegView {
    const pm::TokenBook* book;
    const pm::MarketRules* rules;
    Ns updated;          // last book change
    bool frozen;         // unknown orders on the token
    Px own_bid, own_ask;  // our resting best prices, -1 if none
    // Shares we already took at a level the feed has not updated since (`book_bid` side); null: none.
    Qty (*taken)(const void* venue, std::uint32_t token, bool book_bid, Px px) = nullptr;
    const void* venue = nullptr;
    std::uint32_t token = 0;
};

struct Plan {
    ArbFilter why;
    Qty size;  // sets
    Usd net;   // after fees and allowances, at that size
};

inline Qty book_qty(double shares) { return static_cast<Qty>(shares * 1e6 + 0.5); }
// Shares at a level still there for us.
inline Qty level_qty(const LegView& v, bool book_bid, Px px) {
    const Qty q = book_qty(v.book->size_at(book_bid, px)) - (v.taken ? v.taken(v.venue, v.token, book_bid, px) : 0);
    return q > 0 ? q : 0;
}
inline Qty floor_hundredths(Qty q) { return q / (kShare / 100) * (kShare / 100); }

// Buy side: every leg's best ask, sum below 1. Sell side (pairs, after a split): every leg's best
// bid, sum above 1. `opened`: when the window opened; `room`: what the group cap still allows.
inline Plan plan(bool buy, const LegView* legs, std::size_t n, Ns now, Ns opened, Usd cash, Usd room, const ArbParams& p) {
    if (now - opened < p.min_age) return {ArbFilter::Young, 0, 0};
    for (std::size_t i = 0; i < n; ++i)
        if (now - legs[i].updated > p.fresh) return {ArbFilter::Stale, 0, 0};
    for (std::size_t i = 0; i < n; ++i)
        if (legs[i].frozen) return {ArbFilter::Frozen, 0, 0};
    Px sum = 0, allowance = 0;
    Qty depth = p.max_set, min_qty = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const pm::TokenBook& b = *legs[i].book;
        const Px px = buy ? b.best_ask() : b.best_bid();
        if (px < 0) return {ArbFilter::Edge, 0, 0};
        if (buy ? legs[i].own_ask >= 0 && px >= legs[i].own_ask : legs[i].own_bid >= 0 && px <= legs[i].own_bid)
            return {ArbFilter::SelfCross, 0, 0};
        sum += px;
        const Qty at = level_qty(legs[i], !buy, px);
        if (at < depth) depth = at;
        const pm::MarketRules& r = *legs[i].rules;
        allowance += (p.slip_ticks + p.latency_ticks) * (r.tick > 0 ? r.tick : 1);
        if (r.min_qty > min_qty) min_qty = r.min_qty;
    }
    const Px cost = buy ? sum : kPxOne;  // cash per set: the legs, or one dollar for the split
    Qty size = depth;
    if (cash / cost < size) size = cash / cost;
    if (room / cost < size) size = room / cost;
    size = floor_hundredths(size);
    if (size < min_qty || size <= 0) return {ArbFilter::Size, size, 0};
    Usd net = notional(buy ? kPxOne - sum : sum - kPxOne, size) - notional(allowance, size);
    for (std::size_t i = 0; i < n; ++i) {
        const pm::TokenBook& b = *legs[i].book;
        net -= taker_fee(size, buy ? b.best_ask() : b.best_bid(), *legs[i].rules);
    }
    if (net < notional(p.min_edge, size)) return {ArbFilter::Edge, size, net};
    return {ArbFilter::Pass, size, net};
}

// Cost of buying `q` at the asks (or proceeds of selling at the bids), walking levels best first;
// `limit` gets the last level's price. -1 if the book is not deep enough.
inline Usd walk(const LegView& v, bool buy, Qty q, Px* limit) {
    const pm::TokenBook& b = *v.book;
    Usd u = 0;
    for (Px px = buy ? b.best_ask() : b.best_bid(); px >= 0 && q > 0;
         px = buy ? b.ask_at_or_above(px + 1) : b.bid_at_or_below(px - 1)) {
        const Qty at = level_qty(v, !buy, px);
        const Qty take = at < q ? at : q;
        u += notional(px, take), q -= take, *limit = px;
    }
    return q > 0 ? -1 : u;
}

enum class ArbAction : std::uint8_t { None, Complete, Unwind, Freeze };

struct Choice {
    ArbAction action;
    Usd value;  // attempt PnL after the action, sets valued at 1
};

// X7: legs hold `held` shares and the attempt's cash flow so far is `cash`. Completing buys
// every leg up to the largest holding, unwinding sells every leg down to the smallest; the one
// worth more wins, unless even that loses more than `bound` (R10) or neither is possible.
inline Choice choose(const Qty* held, const LegView* legs, std::size_t n, Usd cash, Usd bound) {
    Qty hi = held[0], lo = held[0];
    for (std::size_t i = 1; i < n; ++i) hi = held[i] > hi ? held[i] : hi, lo = held[i] < lo ? held[i] : lo;
    if (hi == lo) return {ArbAction::None, cash + notional(kPxOne, hi)};
    Usd complete = cash + notional(kPxOne, hi), unwind = cash + notional(kPxOne, lo);
    bool can_complete = true, can_unwind = true;
    for (std::size_t i = 0; i < n; ++i) {
        Px limit = 0;
        if (held[i] < hi) {
            const Usd c = walk(legs[i], true, hi - held[i], &limit);
            if (c < 0) can_complete = false;
            complete -= c + taker_fee(hi - held[i], limit, *legs[i].rules);
        }
        if (held[i] > lo) {
            const Usd c = walk(legs[i], false, held[i] - lo, &limit);
            if (c < 0) can_unwind = false;
            unwind += c - taker_fee(held[i] - lo, limit, *legs[i].rules);
        }
    }
    Choice best{ArbAction::Freeze, cash};
    if (can_complete) best = {ArbAction::Complete, complete};
    if (can_unwind && (!can_complete || unwind > complete)) best = {ArbAction::Unwind, unwind};
    if (best.action == ArbAction::Freeze || best.value < -bound) return {ArbAction::Freeze, best.value};
    return best;
}

struct ArbStats {
    std::uint64_t windows = 0, attempts = 0, empty = 0, complete = 0, completed = 0, unwound = 0;
    std::uint64_t frozen_bound = 0, frozen_depth = 0, frozen_retries = 0;  // why a set was frozen (X7, R10)
    std::uint64_t splits = 0, merges = 0;
    std::uint64_t filtered[static_cast<std::size_t>(ArbFilter::kCount)] = {};  // windows never attempted, by last filter
    Usd realised = 0;  // attempts that ended flat: their whole cash flow, merges included
};

// Runs one attempt per group at a time (X9). The driver reports orders and settlements as they
// happen and calls process() after delivering venue reports: decisions are taken there, never
// inside the order manager's callbacks. Ctx gives the executor its view and its actions:
//   LegView view(token); std::uint64_t submit(token, side, tif, px, qty, tag) (0: risk refused);
//   void cancel(cl); void split(yes, no, qty); void merge(yes, no, qty); Usd cash(); Usd room(token);
//   void kill().
class ArbExec {
   public:
    enum class State : std::uint8_t { Idle, Split, Legs, Resolve, Merge, Frozen };

    explicit ArbExec(const ArbParams& p) : p_(p) {}

    // Setup only. A pair is the Yes and No of one market: it can merge and split (D17); any other
    // group only buys complete sets and holds them (D10).
    void add_group(const std::vector<std::uint32_t>& tokens, bool pair) {
        if (tokens.size() < 2 || tokens.size() > 255) throw std::runtime_error("arb group needs 2..255 legs");
        Group g;
        g.pair = pair;
        for (const std::uint32_t t : tokens) {
            g.legs.push_back({t, 0, 0});
            if (t >= by_token_.size()) by_token_.resize(t + 1), held_.resize(t + 1, 0);
            by_token_[t].push_back(static_cast<std::uint32_t>(groups_.size()));
        }
        g.views.resize(tokens.size());
        g.held.resize(tokens.size());
        groups_.push_back(std::move(g));
        dirty_.reserve(groups_.size());
    }

    // No new attempts (the kill switch tripped); open ones still finish.
    void halt() { p_.on = false; }

    // Shares the executor holds in a token, over all its groups (the maker leaves them alone).
    Qty held(std::uint32_t t) const { return t < held_.size() ? held_[t] : 0; }
    const ArbStats& stats() const { return stats_; }
    State state(std::uint32_t g) const { return groups_[g].st; }
    std::size_t groups() const { return groups_.size(); }
    // Complete sets held (event groups wait for resolution) and their cash flow so far.
    Qty sets_held() const {
        Qty n = 0;
        for (const Group& g : groups_) n += min_held(g);
        return n;
    }
    // Groups holding unequal legs that are not being worked on or frozen: a silent residual.
    std::size_t residual_groups() const {
        std::size_t n = 0;
        for (const Group& g : groups_) {
            Qty hi = g.held[0], lo = g.held[0];
            for (const Qty h : g.held) hi = h > hi ? h : hi, lo = h < lo ? h : lo;
            n += hi != lo && (g.st == State::Idle || g.st == State::Merge);
        }
        return n;
    }
    Usd open_cash() const {
        Usd u = 0;
        for (const Group& g : groups_) u += g.cash;
        return u;
    }

    // A book changed: re-check every group holding `token`, and start an attempt on a window that
    // passes the filters.
    template <class Ctx>
    void on_book(std::uint32_t token, Ns now, Ctx& c) {
        if (token >= by_token_.size()) return;
        for (const std::uint32_t gi : by_token_[token]) check(gi, now, c);
    }

    void on_fill(std::uint32_t tag, Side side, Px px, Qty q, const pm::MarketRules& rules) {
        Group& g = groups_[tag >> 8];
        const std::uint32_t i = tag & 0xff;
        const Usd fee = taker_fee(q, px, rules);
        g.filled = true;
        if (side == Side::Buy) add(g, i, q), g.cash -= notional(px, q) + fee;
        else add(g, i, -q), g.cash += notional(px, q) - fee;
    }
    void on_done(std::uint32_t tag, std::uint64_t cl) {
        Group& g = groups_[tag >> 8];
        Leg& l = g.legs[tag & 0xff];
        if (l.cl != cl) return;
        l.cl = 0;
        mark(tag >> 8);
    }
    // Settlement of one of our fills (D16): a failed one is undone, as the ledger undoes it.
    void on_settle(std::uint32_t tag, const Ledger::Fill& f, bool ok) {
        Group& g = groups_[tag >> 8];
        const std::uint32_t i = tag & 0xff;
        const bool buy = f.side == Side::Buy;
        if (ok) {
            if (buy) g.legs[i].confirmed += f.qty;
            return;
        }
        if (buy) add(g, i, -f.qty), g.cash += notional(f.px, f.qty) + f.fee;
        else add(g, i, f.qty), g.legs[i].confirmed += f.qty, g.cash -= notional(f.px, f.qty) - f.fee;
        mark(tag >> 8);
    }

    bool dirty() const { return !dirty_.empty(); }
    // Decisions for groups whose orders or holdings changed.
    template <class Ctx>
    void process(Ns now, Ctx& c) {
        while (!dirty_.empty()) {
            const std::uint32_t gi = dirty_.back();
            dirty_.pop_back();
            groups_[gi].dirty = false;
            Group& g = groups_[gi];
            if (busy(g)) continue;
            if (g.st == State::Legs && g.rest_pending) {  // policy Q: the thinnest leg went first
                g.rest_pending = false;
                if (g.held[g.first] >= g.size) {
                    for (std::uint32_t i = 0; i < g.legs.size(); ++i)
                        if (i != g.first) send(g, gi, i, Side::Buy, Tif::Fak, c.view(g.legs[i].token).book->best_ask(), g.size, c);
                    if (busy(g)) continue;
                }
            }
            if (g.st != State::Split) evaluate(gi, now, c);
        }
    }

    // Every risk tick: relayer delays, leg timeouts, frozen groups, the R15 hold limit.
    template <class Ctx>
    void on_timer(Ns now, Ctx& c) {
        for (std::uint32_t gi = 0; gi < groups_.size(); ++gi) {
            Group& g = groups_[gi];
            if (g.st == State::Split && now >= g.due) do_split(gi, now, c);
            if (g.st == State::Merge && now >= g.due) do_merge(gi, c);
            if ((g.st == State::Legs || g.st == State::Resolve) && now > g.deadline && !g.cancelled) {
                g.cancelled = true;
                for (const Leg& l : g.legs)
                    if (l.cl) c.cancel(l.cl);
            }
            if (g.st == State::Frozen && now >= g.due) mark(gi), g.due = now + kRetry;
            // R15: money in legs that no complete set covers.
            const Usd exposure = -(g.cash + notional(kPxOne, min_held(g)));
            if (g.incomplete_since && exposure > p_.attempt_bound) {
                if (!g.exposed_since) g.exposed_since = now;
                if (now - g.exposed_since > p_.hold_limit && !g.killed) g.killed = true, c.kill();
            } else {
                g.exposed_since = 0;
            }
        }
    }

   private:
    struct Leg {
        std::uint32_t token;
        std::uint64_t cl;  // order in flight, 0 if none
        Qty confirmed;     // bought shares whose settlement succeeded, not yet merged
    };
    struct Group {
        std::vector<Leg> legs;
        std::vector<LegView> views;
        std::vector<Qty> held;  // shares held per leg
        bool pair = false, buy = true, dirty = false, rest_pending = false, cancelled = false, killed = false, unwinding = false;
        bool filled = false;  // the attempt traded at all
        bool froze = false;   // the attempt was frozen at some point
        bool attempted[2] = {false, false};  // the open window was attempted
        State st = State::Idle;
        ArbFilter last[2] = {ArbFilter::Pass, ArbFilter::Pass};
        Ns opened[2] = {0, 0};  // window open since, 0 if closed; [0] buy, [1] sell
        Ns due = 0, deadline = 0, incomplete_since = 0, exposed_since = 0;
        Qty size = 0;
        Usd cash = 0;
        std::uint32_t first = 0, rounds = 0;
    };

    static Qty min_held(const Group& g) {
        Qty n = g.held[0];
        for (const Qty h : g.held) n = h < n ? h : n;
        return n;
    }
    static bool busy(const Group& g) {
        for (const Leg& l : g.legs)
            if (l.cl) return true;
        return false;
    }
    void add(Group& g, std::uint32_t i, Qty q) { g.held[i] += q, held_[g.legs[i].token] += q; }
    void mark(std::uint32_t gi) {
        if (!groups_[gi].dirty) groups_[gi].dirty = true, dirty_.push_back(gi);
    }

    template <class Ctx>
    void send(Group& g, std::uint32_t gi, std::uint32_t i, Side side, Tif tif, Px px, Qty q, Ctx& c) {
        g.legs[i].cl = px > 0 && q > 0 ? c.submit(g.legs[i].token, side, tif, px, q, gi << 8 | i) : 0;
        if (!g.legs[i].cl) mark(gi);  // refused: nothing will report on it
    }

    template <class Ctx>
    void check(std::uint32_t gi, Ns now, Ctx& c) {
        Group& g = groups_[gi];
        for (std::uint32_t i = 0; i < g.legs.size(); ++i) g.views[i] = c.view(g.legs[i].token);
        for (int s = 0; s < (g.pair ? 2 : 1); ++s) {
            const bool buy = s == 0;
            Px sum = 0;
            bool all = true;
            for (const LegView& v : g.views) {
                const Px px = buy ? v.book->best_ask() : v.book->best_bid();
                all = all && px >= 0, sum += px;
            }
            const bool open = all && (buy ? sum < kPxOne : sum > kPxOne);
            if (!open) {
                if (g.opened[s] && !g.attempted[s]) ++stats_.filtered[static_cast<std::size_t>(g.last[s])];
                g.opened[s] = 0;
                continue;
            }
            if (!g.opened[s]) g.opened[s] = now, g.attempted[s] = false, ++stats_.windows;
            if (g.st != State::Idle || !p_.on || g.attempted[s]) continue;  // one attempt per window
            const Plan pl = plan(buy, g.views.data(), g.views.size(), now, g.opened[s], c.cash(), c.room(g.legs[0].token), p_);
            g.last[s] = pl.why;
            if (pl.why != ArbFilter::Pass) continue;
            g.attempted[s] = true, ++stats_.attempts;
            g.buy = buy, g.size = pl.size, g.rounds = 0, g.filled = false, g.froze = false, g.cancelled = false, g.deadline = now + p_.leg_timeout;
            if (!buy) {
                g.st = State::Split, g.due = now + p_.merge_delay;
                if (p_.merge_delay == 0) do_split(gi, now, c);
                return;
            }
            g.st = State::Legs;
            if (p_.sequential) {
                std::uint32_t thin = 0;
                double least = 1e300;
                for (std::uint32_t i = 0; i < g.legs.size(); ++i) {
                    const pm::TokenBook& b = *g.views[i].book;
                    if (b.size_at(false, b.best_ask()) < least) least = b.size_at(false, b.best_ask()), thin = i;
                }
                g.first = thin, g.rest_pending = true;
                send(g, gi, thin, Side::Buy, Tif::Fok, g.views[thin].book->best_ask(), g.size, c);
            } else {
                for (std::uint32_t i = 0; i < g.legs.size(); ++i) send(g, gi, i, Side::Buy, Tif::Fak, g.views[i].book->best_ask(), g.size, c);
            }
            return;
        }
    }

    // Sell side (D17): one dollar per set becomes a Yes and a No, then both are sold at the bids
    // if the window is still there; otherwise the set is merged back.
    template <class Ctx>
    void do_split(std::uint32_t gi, Ns now, Ctx& c) {
        Group& g = groups_[gi];
        c.split(g.legs[0].token, g.legs[1].token, g.size);
        ++stats_.splits;
        g.cash -= notional(kPxOne, g.size);
        for (std::uint32_t i = 0; i < 2; ++i) add(g, i, g.size), g.legs[i].confirmed += g.size;
        g.st = State::Legs, g.deadline = now + p_.leg_timeout;
        for (std::uint32_t i = 0; i < 2; ++i) g.views[i] = c.view(g.legs[i].token);
        const Px b0 = g.views[0].book->best_bid(), b1 = g.views[1].book->best_bid();
        if (b0 >= 0 && b1 >= 0 && b0 + b1 > kPxOne)
            for (std::uint32_t i = 0; i < 2; ++i) send(g, gi, i, Side::Sell, Tif::Fak, g.views[i].book->best_bid(), g.size, c);
        else
            mark(gi);
    }

    template <class Ctx>
    void do_merge(std::uint32_t gi, Ctx& c) {
        Group& g = groups_[gi];
        Qty n = g.held[0] < g.held[1] ? g.held[0] : g.held[1];
        for (const Leg& l : g.legs) n = l.confirmed < n ? l.confirmed : n;
        if (n <= 0) return;  // waiting for settlement
        c.merge(g.legs[0].token, g.legs[1].token, n);
        ++stats_.merges;
        g.cash += notional(kPxOne, n);
        for (std::uint32_t i = 0; i < 2; ++i) add(g, i, -n), g.legs[i].confirmed -= n;
        if (min_held(g) == 0 && g.held[0] == g.held[1]) finish(g);
    }

    void finish(Group& g) {
        stats_.realised += g.cash;
        g.cash = 0;
        g.st = State::Idle;
    }

    template <class Ctx>
    void evaluate(std::uint32_t gi, Ns now, Ctx& c) {
        Group& g = groups_[gi];
        for (std::uint32_t i = 0; i < g.legs.size(); ++i) g.views[i] = c.view(g.legs[i].token);
        const Choice ch = choose(g.held.data(), g.views.data(), g.legs.size(), g.cash, p_.attempt_bound);
        if (ch.action == ArbAction::None) {
            if (g.st == State::Legs || g.st == State::Resolve) {
                if (g.rounds == 0) ++(g.filled ? stats_.complete : stats_.empty);
                else ++(g.unwinding ? stats_.unwound : stats_.completed);
            }
            g.incomplete_since = 0;
            if (min_held(g) == 0) finish(g);
            else if (g.pair) g.st = State::Merge, g.due = now + p_.merge_delay;
            else g.st = State::Idle;  // complete sets of an event, held to resolution
            if (g.st == State::Merge && p_.merge_delay == 0) do_merge(gi, c);
            return;
        }
        if (!g.incomplete_since) g.incomplete_since = now;
        if (g.st == State::Frozen) g.rounds = 0;  // a retry of a frozen set gets fresh rounds
        if (ch.action == ArbAction::Freeze || g.rounds >= kRounds) {
            if (g.st != State::Frozen && !g.froze)
                ++(g.rounds >= kRounds ? stats_.frozen_retries : ch.value < -p_.attempt_bound ? stats_.frozen_bound : stats_.frozen_depth);
            g.st = State::Frozen, g.due = now + kRetry, g.froze = true;
            return;
        }
        Qty hi = g.held[0], lo = g.held[0];
        for (const Qty h : g.held) hi = h > hi ? h : hi, lo = h < lo ? h : lo;
        g.unwinding = ch.action == ArbAction::Unwind;
        g.st = State::Resolve, ++g.rounds, g.cancelled = false, g.deadline = now + p_.leg_timeout;
        for (std::uint32_t i = 0; i < g.legs.size(); ++i) {
            const Qty q = g.unwinding ? g.held[i] - lo : hi - g.held[i];
            Px limit = 0;
            if (q > 0 && walk(g.views[i], !g.unwinding, q, &limit) >= 0)
                send(g, gi, i, g.unwinding ? Side::Sell : Side::Buy, Tif::Fak, limit, q, c);
        }
    }

    static constexpr std::uint32_t kRounds = 3;     // complete or unwind rounds before a set is frozen
    static constexpr Ns kRetry = 1'000'000'000;     // a frozen set is looked at again this often

    ArbParams p_;
    std::vector<Group> groups_;
    std::vector<std::vector<std::uint32_t>> by_token_;
    std::vector<Qty> held_;
    std::vector<std::uint32_t> dirty_;
    ArbStats stats_;
};

}  // namespace hft::exec
