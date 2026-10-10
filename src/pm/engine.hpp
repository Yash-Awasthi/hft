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

    explicit Engine(const EngineParams& p) : p_(p) {}

    // Token index for an id, created on first sight.
    std::uint32_t token(std::string_view id) {
        const std::uint32_t i = ids_.get(id);
        if (i == toks_.size()) {
            toks_.push_back(std::make_unique<Tok>(p_.maker));
            toks_.back()->maker.set_enabled(p_.quote);
        }
        return i;
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
                t.maker.on_snapshot(ns);
                after(ns, i);
                break;
            }
            case Kind::PriceChange:
                touched_.clear();
                for (const auto& c : e.changes) {
                    const std::uint32_t i = token(c.asset);
                    if (!toks_[i]->seeded) continue;
                    toks_[i]->maker.book.set(c.buy, c.px, c.size);
                    if (std::find(touched_.begin(), touched_.end(), i) == touched_.end()) touched_.push_back(i);
                }
                for (const std::uint32_t i : touched_) {
                    toks_[i]->maker.on_level(ns);
                    after(ns, i);
                }
                break;
            case Kind::Trade: {
                const std::uint32_t i = token(e.asset);
                Tok& t = *toks_[i];
                if (!t.seeded) break;
                const double inv = t.maker.inventory();
                t.maker.on_trade(ns, e.buy, e.px, e.size);
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
    struct Tok {
        explicit Tok(const MakerParams& p) : maker(p) {}
        TokenMaker maker;
        std::uint32_t conn = ~0u;
        bool seeded = false;
        std::int32_t bid = -1, ask = -1;  // last logged quotes
    };
    struct Conn {
        std::int64_t last_ns = 0;
        bool stale = false;
    };

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
