#pragma once

// Trading core shared by live and replay runs: per-token books and makers, portfolio risk,
// the arbitrage scanner and a decision log. Its output depends only on the sequence of
// (receive time, event) it is given, so a recorded session replays to the same decisions.
//
// Control events come from the feed layer as ordinary records: "_heartbeat" (the
// connection is alive) and "_reconnect" (books on the connection are stale until their
// next snapshot).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "exec/oms.hpp"
#include "exec/sim_venue.hpp"
#include "pm/arb.hpp"
#include "pm/maker.hpp"
#include "pm/msg.hpp"

namespace hft::pm {

struct EngineParams {
    MakerParams maker;
    bool quote = true;            // run the makers; off: books and scanner only
    double max_gross_shares = 5000;  // sum of |inventory| over tokens before all quoting pauses
    double loss_stop_usd = 100;   // total PnL below -loss_stop halts quoting for the session
    double stale_s = 20;          // a connection silent this long pauses its tokens' quotes
    double tick_s = 0.1;          // risk checks run when receive time crosses this grid
    // Venue mode: makers quote through risk, the order manager and the simulated venue, which
    // decides fills; off: each maker fills itself (its own model, as before).
    bool venue = false;
    exec::SimConfig sim;
    exec::RiskLimits risk;
    bool long_only = true;
};

struct Decision {
    enum Kind : std::uint8_t { Quote, Fill, Pause, Resume, Halt } kind;
    std::int64_t ns;
    std::uint32_t token;  // ~0u for portfolio-wide decisions
    std::int32_t bid, ask;
    double inventory;
};

class Engine {
   public:
    static constexpr std::uint32_t kAll = ~0u;

    explicit Engine(const EngineParams& p)
        : p_(p),
          ledger_(1'000'000 * exec::kDollar, p.long_only),
          risk_(p.risk),
          oms_(1, risk_, exp_, ledger_),
          sim_(p.sim, BookOf{this}) {}

    // Token index for an id, created on first sight.
    std::uint32_t token(std::string_view id) {
        const std::uint32_t i = ids_.get(id);
        if (i == toks_.size()) {
            toks_.push_back(std::make_unique<Tok>(p_.maker));
            toks_.back()->maker.set_enabled(p_.quote);
            toks_.back()->maker.fill_log = fill_log_;
            toks_.back()->maker.token_index = i;
            toks_.back()->maker.set_venue_fills(p_.venue);
            if (p_.venue) {
                const std::uint32_t n = i + 1;
                ledger_.ensure(n), exp_.ensure(n, 1), risk_.ensure(n), oms_.ensure(n), sim_.ensure(n);
                ledger_.set_group(i, 0), exp_.set_group(i, 0), risk_.set_group(i, 0);
                rules_.resize(n);
                sim_.set_rules(i, rules_[i]);
            }
        }
        return i;
    }
    // A market's order rules (session file), for the venue and risk checks.
    void set_rules(std::uint32_t t, const MarketRules& r) {
        if (!p_.venue) return;
        rules_[t] = r;
        sim_.set_rules(t, r);
    }
    void set_conn(std::uint32_t t, std::uint32_t conn) {
        toks_[t]->conn = conn;
        if (conn >= conns_.size()) conns_.resize(conn + 1);
    }
    std::uint32_t add_group(std::string name, const std::vector<std::string>& ids) {
        std::vector<std::uint32_t> ts;
        for (const auto& id : ids) ts.push_back(token(id));
        return arb_.add_group(std::move(name), std::move(ts));
    }

    void on_event(std::int64_t ns, const Event& e) {
        if (ns >= next_tick_) tick(ns);
        ++events_;
        switch (e.kind) {
            case Kind::Book: {
                const std::uint32_t i = token(e.asset);
                Tok& t = *toks_[i];
                t.maker.book.clear();
                for (const auto& l : e.bids) t.maker.book.set(true, l.px, l.size);
                for (const auto& l : e.asks) t.maker.book.set(false, l.px, l.size);
                if (e.tick > 0) t.maker.tick = e.tick;
                t.seeded = true;
                if (p_.venue) sim_.on_snapshot(i);
                t.maker.on_snapshot(ns);
                sync(i, ns);
                after(ns, i);
                break;
            }
            case Kind::PriceChange:
                touched_.clear();
                for (const auto& c : e.changes) {
                    const std::uint32_t i = token(c.asset);
                    if (!toks_[i]->seeded) continue;
                    toks_[i]->maker.book.set(c.buy, c.px, c.size);
                    if (p_.venue) sim_.on_level(i, c.buy, c.px);
                    if (std::find(touched_.begin(), touched_.end(), i) == touched_.end()) touched_.push_back(i);
                }
                for (const std::uint32_t i : touched_) {
                    toks_[i]->maker.on_level(ns);
                    sync(i, ns);
                    after(ns, i);
                }
                break;
            case Kind::Trade: {
                const std::uint32_t i = token(e.asset);
                Tok& t = *toks_[i];
                if (!t.seeded) break;
                const double inv = t.maker.inventory();
                in_trade_ = true;  // fills delivered now are logged below, as the makers' own model logs them
                if (p_.venue) sim_.on_trade(i, e.px, e.size, e.buy, ns), pump(ns);
                t.maker.on_trade(ns, e.buy, e.px, e.size);
                sync(i, ns);
                in_trade_ = false;
                ++trades_;
                if (t.maker.inventory() != inv) {
                    ++fills_;
                    log_.push_back({Decision::Fill, ns, i, e.px, 0, t.maker.inventory()});
                }
                after(ns, i);
                break;
            }
            case Kind::Other:
                if (e.conn >= 0 && static_cast<std::size_t>(e.conn) < conns_.size()) {
                    Conn& c = conns_[static_cast<std::size_t>(e.conn)];
                    c.last_ns = ns;
                    if (e.type == "_reconnect") {
                        ++reconnects_;
                        for (std::uint32_t i = 0; i < toks_.size(); ++i)
                            if (toks_[i]->conn == static_cast<std::uint32_t>(e.conn)) toks_[i]->seeded = false, toks_[i]->maker.book.clear();
                    }
                }
                break;
        }
    }

    // Portfolio risk at receive time `ns`: loss stop, gross inventory, stale connections.
    void tick(std::int64_t ns) {
        next_tick_ = ns + static_cast<std::int64_t>(p_.tick_s * 1e9);
        if (p_.venue) {  // order timeouts, freeing finished orders, position reconciliation (R14)
            auto sink = [&](const exec::VenueReq& r) { sim_.request(r, ns); };
            oms_.on_timer(ns, sink);
            pump(ns);
            // Fills in flight (latency, settlement, a held connection) make positions differ for
            // a while; only a difference that outlasts all of them is a missing fill.
            const std::int64_t grace = p_.sim.settle_delay + 2 * (p_.sim.lat_in + p_.sim.lat_out + p_.sim.jitter) +
                                       p_.sim.disc_for + 1'000'000'000;
            for (std::uint32_t t = 0; t < toks_.size(); ++t) {
                std::int64_t& since = toks_[t]->mismatch_since;
                if (sim_.position(t) == ledger_.pos(t)) since = 0;
                else if (!since) since = ns;
                else if (since > 0 && ns - since > grace) ++mismatches_, since = -1, risk_.kill(exec::KillReason::Mismatch, ns);
            }
            if (risk_.killed() && !cancelled_all_) {
                cancelled_all_ = true;
                oms_.cancel_all(ns, sink);
                pump(ns);
            }
        }
        if (!p_.quote) return;
        double pnl = 0, gross = 0;
        for (const auto& t : toks_) pnl += t->maker.pnl(), gross += std::abs(t->maker.inventory());
        pnl_ = pnl, gross_ = gross;
        if (!halted_ && pnl < -p_.loss_stop_usd) {
            halted_ = true;
            log_.push_back({Decision::Halt, ns, kAll, 0, 0, gross});
        }
        const bool gross_ok = gross < p_.max_gross_shares;
        for (std::size_t c = 0; c < conns_.size(); ++c)
            conns_[c].stale = conns_[c].last_ns && static_cast<double>(ns - conns_[c].last_ns) * 1e-9 > p_.stale_s;
        for (std::uint32_t i = 0; i < toks_.size(); ++i) {
            Tok& t = *toks_[i];
            const bool stale = t.conn < conns_.size() && conns_[t.conn].stale;
            const bool on = !halted_ && gross_ok && !stale;
            if (on != t.maker.enabled()) {
                t.maker.set_enabled(on);
                sync(i, ns);
                log_.push_back({on ? Decision::Resume : Decision::Pause, ns, i, 0, 0, t.maker.inventory()});
                after(ns, i);
            }
        }
    }

    // Decisions since the last drain, oldest first; the caller clears it.
    std::vector<Decision>& decisions() { return log_; }

    // Closes open arbitrage windows at the end of a run.
    void finish(std::int64_t ns) {
        arb_.finish(ns, [&](const ArbWindow& w) { closed_.push_back(w); });
    }

    // Every maker fill goes to `log` (tokens created later too).
    void set_fill_log(std::vector<MakerFill>* log) {
        fill_log_ = log;
        for (auto& t : toks_) t->maker.fill_log = log;
    }

    const exec::Ledger& ledger() const { return ledger_; }
    const exec::Oms& oms() const { return oms_; }
    const exec::SimStats& venue_stats() const { return sim_.stats(); }
    const exec::Risk& exec_risk() const { return risk_; }
    std::uint64_t position_mismatches() const { return mismatches_; }

    // Read-only views for reports and metrics.
    std::size_t tokens() const { return toks_.size(); }
    const std::string& token_id(std::uint32_t i) const { return ids_.key(i); }
    const TokenMaker& maker(std::uint32_t i) const { return toks_[i]->maker; }
    bool seeded(std::uint32_t i) const { return toks_[i]->seeded; }
    const ArbScanner& arb() const { return arb_; }
    std::vector<ArbWindow>& arb_closed() { return closed_; }
    std::uint64_t events() const { return events_; }
    std::uint64_t trades() const { return trades_; }
    std::uint64_t fills() const { return fills_; }
    std::uint64_t reconnects() const { return reconnects_; }
    double pnl() const { return pnl_; }
    double gross() const { return gross_; }
    bool halted() const { return halted_; }

   private:
    struct Live {
        std::uint64_t cl = 0, seq = 0;  // our order on this side, and the maker placement it is
    };
    struct Tok {
        explicit Tok(const MakerParams& p) : maker(p) {}
        TokenMaker maker;
        Live live[2];  // [0] bid, [1] ask
        std::int64_t mismatch_since = 0;  // venue and ledger positions differ since; -1 once reported
        std::uint32_t conn = ~0u;
        bool seeded = false;
        std::int32_t bid = -1, ask = -1;  // last logged quotes
    };
    struct Conn {
        std::int64_t last_ns = 0;
        bool stale = false;
    };

    struct BookOf {
        Engine* e;
        const TokenBook& operator()(std::uint32_t t) const { return e->toks_[t]->maker.book; }
    };

    // Venue mode: brings our orders in line with the maker's quotes. A side whose quote went
    // away or was placed again is cancelled first (both sides, so a new bid never meets our old
    // ask), then new orders go in; the venue answers at once at zero latency.
    void sync(std::uint32_t i, std::int64_t ns) {
        if (!p_.venue) return;
        Tok& t = *toks_[i];
        const std::int32_t px[2] = {t.maker.bid_px(), t.maker.ask_px()};
        const std::uint64_t seq[2] = {t.maker.bid_seq(), t.maker.ask_seq()};
        auto sink = [&](const exec::VenueReq& r) { sim_.request(r, ns); };
        bool cancelled = false;
        for (int s = 0; s < 2; ++s)
            if (t.live[s].cl && (px[s] < 0 || seq[s] != t.live[s].seq)) cancelled |= oms_.cancel(t.live[s].cl, ns, sink), t.live[s].cl = 0;
        if (cancelled) pump(ns);
        for (int s = 0; s < 2; ++s) {
            if (px[s] < 0 || seq[s] == t.live[s].seq) continue;
            exec::Qty q = static_cast<exec::Qty>(t.maker.size() * 1e6 + 0.5);
            if (s && p_.long_only) {  // D27: asks only up to held shares, in 2-dp sizes; retried at the next sync
                q = std::min(q, ledger_.available_pos(i) / 10'000 * 10'000);
                if (q < rules_[i].min_qty) continue;
            }
            t.live[s].seq = seq[s];
            const exec::OrderIntent in{i, s ? exec::Side::Sell : exec::Side::Buy, exec::Tif::Gtc, true, 0, px[s], q, 0};
            const exec::MarketView mv{t.maker.book.best_bid(), t.maker.book.best_ask(), t.seeded, !t.seeded, &rules_[i]};
            t.live[s].cl = oms_.submit(in, mv, ns, sink);
        }
        pump(ns);
    }
    // Delivers everything the venue has due at `ns` to the order manager; fills reach the maker.
    void pump(std::int64_t ns) {
        sim_.run(ns, true, [&](const exec::VenueRpt& r) {
            const exec::Ledger::Fill* f = r.kind == exec::VenueRpt::SettleFailed ? ledger_.pending(r.fill_id) : nullptr;
            if (f) toks_[f->token]->maker.on_settle_failed(f->side == exec::Side::Buy, f->px, static_cast<double>(f->qty) * 1e-6);
            const std::uint32_t failed = f ? f->token : kAll;
            oms_.on_report(r, ns, [&](const exec::Order& o, exec::OrdEvent ev, exec::Qty q, exec::Px p) {
                Tok& t = *toks_[o.in.token];
                const int s = o.in.side == exec::Side::Buy ? 0 : 1;
                const bool current = t.live[s].cl == o.cl_id;
                if (ev == exec::OrdEvent::Fill) {
                    t.maker.on_fill(ns, s == 0, p, static_cast<double>(q) * 1e-6, current);
                    own(o.in.token);
                    if (!in_trade_) ++fills_, log_.push_back({Decision::Fill, ns, o.in.token, p, 0, t.maker.inventory()});
                }
                if (current && exec::terminal(o.state)) t.live[s].cl = 0;
            });
            if (failed != kAll) own(failed);
        });
    }
    void own(std::uint32_t t) { toks_[t]->maker.set_inventory(static_cast<double>(ledger_.pos(t)) * 1e-6); }

    // Logs a quote change and re-checks the token's arbitrage groups.
    void after(std::int64_t ns, std::uint32_t i) {
        Tok& t = *toks_[i];
        const std::int32_t b = t.maker.bid_px(), a = t.maker.ask_px();
        if (b != t.bid || a != t.ask) {
            t.bid = b, t.ask = a;
            log_.push_back({Decision::Quote, ns, i, b, a, t.maker.inventory()});
        }
        arb_.on_book(i, ns, [&](std::uint32_t k) -> const TokenBook& { return toks_[k]->maker.book; },
                     [&](const ArbWindow& w) { closed_.push_back(w); });
    }

    EngineParams p_;
    exec::Ledger ledger_;
    exec::Exposure exp_;
    exec::Risk risk_;
    exec::Oms oms_;
    exec::SimVenue<BookOf> sim_;
    std::vector<MarketRules> rules_;
    std::uint64_t mismatches_ = 0;
    bool cancelled_all_ = false, in_trade_ = false;
    std::vector<MakerFill>* fill_log_ = nullptr;
    TokenIndex ids_;
    std::vector<std::unique_ptr<Tok>> toks_;
    std::vector<Conn> conns_;
    std::vector<std::uint32_t> touched_;
    std::vector<Decision> log_;
    std::vector<ArbWindow> closed_;
    ArbScanner arb_;
    std::int64_t next_tick_ = 0;
    std::uint64_t events_ = 0, trades_ = 0, fills_ = 0, reconnects_ = 0;
    double pnl_ = 0, gross_ = 0;
    bool halted_ = false;
};

}  // namespace hft::pm
