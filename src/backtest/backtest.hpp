#pragma once

// Event-driven backtest of one symbol (DESIGN.md section 4). Real market events apply to
// the exchange-side book at exchange time; the strategy sees them a market-data latency
// later, through its own book and features; its orders reach the exchange after the
// processing and order-entry latencies, and reports come back one order-entry latency after
// the exchange produced them. At equal timestamps real market events go first. Accounting
// marks to the real-order mid at exchange time and checks the PnL identity after every event.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "backtest/accounting.hpp"
#include "backtest/risk.hpp"
#include "core/endian.hpp"
#include "data/store.hpp"
#include "engine/replay_exchange.hpp"
#include "engine/scheduler.hpp"
#include "feed/itch.hpp"
#include "strategy/multi_features.hpp"

namespace hft::backtest {

using book::Side;

struct Config {
    engine::Config exchange;
    engine::FillRule fill_rule = engine::FillRule::Queue;
    std::uint64_t market_data_ns = 0, order_entry_ns = 0, processing_ns = 0;
    std::uint64_t start_ns = 34'200'000'000'000;  // 09:30, quoting starts
    std::uint64_t stop_ns = 57'540'000'000'000;   // 15:59, quoting stops, flatten begins
    std::uint64_t end_ns = 57'600'000'000'000;    // 16:00
    std::int64_t sec_fee_per_million = 0;         // micro-dollars per $1M sold (Section 31)
    std::int64_t taf_per_share = 0;               // micro-dollars per share sold (FINRA TAF)
    std::int64_t taf_max = 0;                     // micro-dollars per trade
    RiskLimits risk;
    bool check_identity = true;
};

// An order of ours as the strategy knows it from its own messages and the reports.
struct Working {
    std::uint64_t ref = 0;  // 0 until acknowledged
    std::uint64_t tag = 0;  // our id, known at once
    Side side;
    std::uint32_t price;
    std::uint32_t open;
    bool cancel_sent = false;
    bool ioc = false;       // a take: counts against the position limit until its report returns
    std::uint64_t key = 0;  // largest reference the strategy had seen when it sent the order
    // From the strategy's own feed: shares of real orders ranking ahead (references below
    // key, the exchange assigns them on receipt) and all shares at the order's price.
    double ahead = 0, level = 0;
};

// Everything the strategy may look at when it decides: no exchange-side state.
struct View {
    std::uint64_t now;  // strategy clock
    const book::TickBook& book;
    const double* features;
    std::size_t n_features;
    double mid;  // ticks, NaN when one-sided
    std::int64_t inventory;
    const std::vector<Working>& working;
    bool quoting;  // false outside the trading window and once flattening has begun
};

// What the strategy wants: resting quotes per side (qty 0 for none) and an optional take.
struct Desired {
    std::uint32_t bid_px = 0, bid_qty = 0, ask_px = 0, ask_qty = 0;
    int take_side = 0;  // +1 buy, -1 sell
    std::uint32_t take_qty = 0, take_limit = 0;
};

struct Summary {
    std::int64_t total = 0, spread = 0, inventory_pnl = 0, fees = 0, adverse = 0;
    std::int64_t end_inventory = 0, max_abs_inventory = 0, volume = 0;
    std::uint64_t fills = 0, orders = 0, cancels = 0, rejects = 0, events = 0;
    std::uint64_t fills_during_cancel = 0;  // fills that arrived while our cancel was in flight
    engine::Divergence divergence;
    std::vector<std::int64_t> minute_pnl;  // total PnL at the end of each minute of the window
    std::vector<std::uint64_t> fill_ts;
    std::vector<std::int64_t> fill_signed_shares;
    std::vector<std::uint32_t> fill_price;
    std::vector<std::uint8_t> fill_maker;
};

template <class Strategy>
class Backtest {
   public:
    Backtest(Config c, Strategy& s)
        : c_(c), s_(s), x_(c.exchange, c.fill_rule, 1 << 16), risk_(c.risk) {}

    // Quote changes within this many ticks of a working order keep the order (churn control).
    void set_hysteresis(std::uint32_t ticks) { hysteresis_ = ticks; }

    // target: the traded symbol's locate; index: locates feeding cross-asset features.
    Summary run(const std::string& store, std::uint16_t target, std::vector<std::uint16_t> index) {
        std::vector<std::uint16_t> all{target};
        all.insert(all.end(), index.begin(), index.end());
        std::vector<std::size_t> index_slots;
        for (std::size_t i = 1; i < all.size(); ++i) index_slots.push_back(i);
        strategy::FeatureParams fp;
        fp.tick = c_.exchange.tick;  // the strategy's mid is in exchange ticks
        strategy::MultiFeatures mf(all.size(), index_slots, fp);
        mf_ = &mf;
        row_.assign(mf.count(), 0);

        data::MergedReader rd(store, all, 64);
        data::Record rec{};
        std::uint16_t loc;
        while (rd.next(rec, loc)) {
            const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
            drain(ts, false);
            const std::size_t slot =
                loc == target ? 0
                              : 1 + static_cast<std::size_t>(
                                        std::find(index.begin(), index.end(), loc) - index.begin());
            now_ = ts;
            if (slot == 0) exchange_event(rec.data, rec.len, ts);
            if (rec.len > sizeof(Md::msg)) throw std::runtime_error("message too long");
            Md m{rec.seq, slot, static_cast<std::uint8_t>(rec.len), {}};
            std::memcpy(m.msg, rec.data, rec.len);
            md_.push_back(m);
            q_.push(ts + c_.market_data_ns, engine::Kind::MarketData, md_id_++);
            if (ts >= c_.end_ns) break;
        }
        drain(~0ull, true);
        acct_.flush();
        sum_.total = acct_.total_pnl();
        sum_.spread = acct_.spread_capture();
        sum_.inventory_pnl = acct_.inventory_pnl();
        sum_.fees = acct_.fee_pnl();
        sum_.adverse = acct_.adverse_selection();
        sum_.end_inventory = acct_.inventory();
        sum_.volume = acct_.volume();
        sum_.fills = acct_.fills();
        sum_.divergence = x_.divergence();
        return sum_;
    }

   private:
    struct Md {
        std::uint64_t seq;
        std::size_t slot;
        std::uint8_t len;
        std::uint8_t msg[56];  // the longest ITCH 5.0 message is 50 bytes
    };
    struct Outgoing {
        bool cancel;
        std::uint64_t tag, ref;
        engine::NewOrder order;
    };

    void exchange_event(const std::uint8_t* msg, std::size_t len, std::uint64_t ts) {
        x_.on_itch(msg, len, [&](const engine::Event& e) { on_exchange(e, ts); });
        mark(ts);
        ++sum_.events;
    }

    void mark(std::uint64_t ts) {
        const book::Bbo b = x_.real_bbo();
        if (b.bid_px && b.ask_px && b.ask_px > b.bid_px &&
            (b.bid_px != last_.bid_px || b.ask_px != last_.ask_px)) {
            acct_.mid(b.bid_px, b.ask_px, ts);
            last_ = b;
        }
        if (c_.check_identity && !acct_.identity_holds())
            throw std::logic_error("PnL identity broken");
        while (ts >= c_.start_ns && ts <= c_.end_ns && minute_end() <= ts) {
            sum_.minute_pnl.push_back(acct_.total_pnl());
        }
    }

    std::uint64_t minute_end() const {
        return c_.start_ns + (sum_.minute_pnl.size() + 1) * 60'000'000'000ull;
    }

    // An exchange event about our orders: accounting now, report to the strategy later.
    void on_exchange(const engine::Event& e, std::uint64_t ts) {
        if (e.type == engine::EventType::Fill) {
            const int side = e.side == Side::Buy ? 1 : -1;
            std::int64_t fee = e.fee;
            if (side < 0) {
                const std::int64_t notional = static_cast<std::int64_t>(e.price) * 100 * e.qty;
                fee += notional / 1'000'000 * c_.sec_fee_per_million / 1'000'000;
                fee += std::min(c_.taf_max, c_.taf_per_share * e.qty);
            }
            acct_.fill(side, e.price, e.qty, fee, ts);
            sum_.max_abs_inventory = std::max(sum_.max_abs_inventory, std::abs(acct_.inventory()));
            sum_.fill_ts.push_back(ts);
            sum_.fill_signed_shares.push_back(side * static_cast<std::int64_t>(e.qty));
            sum_.fill_price.push_back(e.price);
            sum_.fill_maker.push_back(e.maker);
        }
        reports_.push_back(e);
        q_.push(ts + c_.order_entry_ns, engine::Kind::Report, rep_id_++);
    }

    void drain(std::uint64_t until, bool all) {
        engine::Timed t;
        while (q_.peek(t) && (all || t.time < until)) {
            q_.pop(t);
            now_ = std::max(now_, t.time);
            switch (t.kind()) {
                case engine::Kind::MarketData:
                    market_data(t.payload, t.time);
                    break;
                case engine::Kind::OrderArrival:
                    arrive(t.payload, t.time);
                    break;
                case engine::Kind::Report:
                    report(t.payload);
                    break;
                default:
                    break;
            }
        }
    }

    void market_data(std::uint32_t id, std::uint64_t now) {
        const Md& m = md_[id - md_done_];
        const std::size_t slot = m.slot;
        const bool changed = mf_->on_itch(slot, m.msg, m.len, m.seq);
        if (slot == 0 && (m.msg[0] == 'A' || m.msg[0] == 'F'))
            max_ref_ = std::max(max_ref_, load_be64(m.msg + 11));
        if (slot == 0 && m.msg[0] == 'U') max_ref_ = std::max(max_ref_, load_be64(m.msg + 19));
        md_.pop_front();  // m is gone from here on
        ++md_done_;
        if (!changed || slot != 0) return;
        queue_positions(mf_->book(0));
        mf_->row(0, mf_->last_event().ts, row_.data());
        const bool quoting = now >= c_.start_ns && now < c_.stop_ns && !risk_.killed();
        const View v{now,        mf_->book(0), row_.data(), row_.size(), mf_->symbol(0).mid(),
                     inventory_, working_,     quoting};
        Desired d;
        if (now >= c_.start_ns && now < c_.end_ns) {
            if (quoting)
                s_.decide(v, d);
            else
                flatten(v, d);
            act(v, d);
        }
    }

    // After the quoting window: no quotes, inventory taken out against the book.
    void flatten(const View& v, Desired& d) {
        const bool take_in_flight =
            std::any_of(working_.begin(), working_.end(), [](const Working& w) { return w.ioc; });
        if (inventory_ == 0 || take_in_flight) return;
        const book::Bbo b = v.book.bbo();
        if (!b.bid_px || !b.ask_px) return;
        d.take_side = inventory_ > 0 ? -1 : 1;
        d.take_qty = static_cast<std::uint32_t>(
            std::min<std::int64_t>(std::abs(inventory_), c_.risk.max_order));
        d.take_limit =
            inventory_ > 0 ? b.bid_px - 5 * c_.exchange.tick : b.ask_px + 5 * c_.exchange.tick;
    }

    // Turns the desired quotes into cancels and new orders; same-price orders are kept.
    void act(const View& v, const Desired& d) {
        const double mid_px = std::isfinite(v.mid) ? v.mid * c_.exchange.tick : 0;
        for (const Side side : {Side::Buy, Side::Sell}) {
            const std::uint32_t px = side == Side::Buy ? d.bid_px : d.ask_px;
            const std::uint32_t qty = side == Side::Buy ? d.bid_qty : d.ask_qty;
            bool have = false;
            for (Working& w : working_) {
                if (w.side != side || w.cancel_sent || w.ioc) continue;
                // Hysteresis: an order within `hysteresis` ticks of the target stays.
                const std::uint32_t gap = w.price > px ? w.price - px : px - w.price;
                if (qty && gap <= hysteresis_ * c_.exchange.tick && !have) {
                    have = true;
                    continue;
                }
                if (!risk_.check_cancel(v.now)) continue;
                w.cancel_sent = true;
                send({true, w.tag, w.ref, {}});
            }
            if (have || !qty || !mid_px) continue;
            place(side, px, qty, engine::Tif::Day, true, mid_px, v.now);
        }
        if (d.take_side && d.take_qty && mid_px)
            place(d.take_side > 0 ? Side::Buy : Side::Sell, d.take_limit, d.take_qty,
                  engine::Tif::Ioc, false, mid_px, v.now);
    }

    void place(Side side, std::uint32_t px, std::uint32_t qty, engine::Tif tif, bool post_only,
               double mid_px, std::uint64_t now) {
        std::int64_t open_buy = 0, open_sell = 0;
        for (const Working& w : working_) (w.side == Side::Buy ? open_buy : open_sell) += w.open;
        if (risk_.check_order(side, px, qty, inventory_, open_buy, open_sell, mid_px,
                              c_.exchange.tick, now, acct_.total_pnl()) != Reject::None) {
            ++sum_.rejects;
            return;
        }
        const std::uint64_t tag = ++next_tag_;
        working_.push_back({0, tag, side, px, qty, false, tif == engine::Tif::Ioc, max_ref_});
        send({false, tag, 0, {1, side, px, qty, tif, post_only}});
        ++sum_.orders;
    }

    void queue_positions(const book::TickBook& b) {
        for (Working& w : working_) {
            if (w.ioc) continue;
            w.level = static_cast<double>(b.level_qty(w.side, w.price));
            const std::uint64_t key = w.key;
            w.ahead = static_cast<double>(b.sum_where(
                w.side, w.price, [key](std::uint64_t ref, std::uint64_t) { return ref < key; }));
        }
    }

    void send(Outgoing o) {
        out_.push_back(o);
        q_.push(now_ + c_.processing_ns + c_.order_entry_ns, engine::Kind::OrderArrival, out_id_++);
    }

    // Our message reaches the exchange.
    void arrive(std::uint32_t id, std::uint64_t ts) {
        const Outgoing o = out_[id - out_done_];
        out_.pop_front();
        ++out_done_;
        auto sink = [&](const engine::Event& e) {
            engine::Event tagged = e;
            tagged.match = o.tag;  // our tag rides along for the report
            if (e.type == engine::EventType::Accepted) exchange_ref_[o.tag] = e.ref;
            on_exchange(tagged, ts);
        };
        if (o.cancel) {
            ++sum_.cancels;
            // The strategy may cancel before its ack arrives; the exchange knows the order.
            const auto it = exchange_ref_.find(o.tag);
            if (it != exchange_ref_.end()) {
                x_.cancel(it->second, 1, sink);
                exchange_ref_.erase(it);
            }
            return;
        }
        x_.submit(o.order, sink);
        mark(ts);
    }

    // A report reaches the strategy.
    void report(std::uint32_t id) {
        const engine::Event e = reports_[id - rep_done_];
        reports_.pop_front();
        ++rep_done_;
        if (e.type == engine::EventType::Accepted) {
            for (Working& w : working_)
                if (w.tag == e.match) w.ref = e.ref;
            return;
        }
        Working* w = nullptr;
        for (Working& x : working_)
            if (x.ref == e.ref && e.ref) w = &x;
        if (e.type == engine::EventType::Fill) {
            inventory_ += (e.side == Side::Buy ? 1 : -1) * static_cast<std::int64_t>(e.qty);
            if (w) {
                sum_.fills_during_cancel += w->cancel_sent;
                w->open -= std::min(w->open, e.qty);
            }
        } else if (e.type == engine::EventType::Cancelled ||
                   e.type == engine::EventType::Rejected) {
            if (w) w->open = 0;
        }
        std::erase_if(working_, [](const Working& x) { return x.ref && x.open == 0; });
        std::erase_if(working_, [&](const Working& x) {
            return !x.ref && e.type == engine::EventType::Rejected && x.tag == e.match;
        });
    }

    Config c_;
    Strategy& s_;
    engine::ReplayExchange<> x_;
    Risk risk_;
    Accounting acct_;
    engine::EventQueue q_{4096};
    strategy::MultiFeatures* mf_ = nullptr;
    std::vector<double> row_;
    std::deque<Md> md_;
    std::deque<Outgoing> out_;
    std::deque<engine::Event> reports_;
    std::uint32_t md_id_ = 0, md_done_ = 0, out_id_ = 0, out_done_ = 0, rep_id_ = 0, rep_done_ = 0;
    std::vector<Working> working_;
    std::unordered_map<std::uint64_t, std::uint64_t> exchange_ref_;  // our tag -> exchange ref
    std::int64_t inventory_ = 0;  // as the strategy knows it from reports
    std::uint64_t next_tag_ = 0, now_ = 0;
    std::uint32_t hysteresis_ = 0;
    std::uint64_t max_ref_ = 0;
    book::Bbo last_{};
    Summary sum_;
};

}  // namespace hft::backtest
