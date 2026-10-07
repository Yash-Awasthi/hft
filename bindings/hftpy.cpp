// Python module for research: features and labels computed by the same C++ code the
// backtest runs. Features and labels are separate calls; join them on `seq`.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "backtest/strategies.hpp"
#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"
#include "strategy/grid.hpp"
#include "strategy/labeler.hpp"
#include "strategy/lifecycles.hpp"
#include "strategy/multi_features.hpp"
#include "strategy/qr_events.hpp"
#include "strategy/trades.hpp"
#include "strategy/event_tokens.hpp"
#include "strategy/event_transformer.hpp"
#include "sources/queue_reactive.hpp"

namespace nb = nanobind;
using namespace hft;

namespace {

template <class T>
nb::ndarray<nb::numpy, T> array(std::vector<T>&& v, std::size_t rows, std::size_t cols = 0) {
    auto* owned = new std::vector<T>(std::move(v));
    nb::capsule del(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
    if (cols == 0) return nb::ndarray<nb::numpy, T>(owned->data(), {rows}, del);
    return nb::ndarray<nb::numpy, T>(owned->data(), {rows, cols}, del);
}

struct Name {
    std::string& out;
    void operator()(const itch::StockDirectory& m) {
        out.assign(m.stock, 8);
        out.erase(out.find_last_not_of(' ') + 1);
    }
    template <class T>
    void operator()(const T&) {}
};

// {symbol: locate} from each symbol file's directory message.
nb::dict symbols(const std::string& store) {
    nb::dict d;
    for (const auto& e : std::filesystem::directory_iterator(store)) {
        if (e.path().extension() != ".idx") continue;
        const auto loc = static_cast<std::uint16_t>(std::stoul(e.path().stem().string()));
        data::SymbolReader rd(store, loc);
        data::Record rec{};
        std::string name;
        Name n{name};
        for (int i = 0; i < 64 && name.empty() && rd.next(rec); ++i)
            if (rec.data[0] == 'R') itch::dispatch(rec.data, rec.len, n);
        if (!name.empty()) d[nb::str(name.c_str())] = loc;
    }
    return d;
}

nb::list feature_names(std::size_t indices) {
    nb::list l;
    for (const char* n : strategy::SymbolFeatures::kNames) l.append(n);
    for (std::size_t k = 0; k < indices; ++k)
        for (double tau : strategy::MultiFeatures::kIndexTau)
            l.append(nb::str(
                ("index" + std::to_string(k) + "_mom_" + std::to_string(int(tau * 1000)) + "ms")
                    .c_str()));
    return l;
}

// Features of each target symbol at every sample_every-th event of that symbol, with the
// index symbols' cross-asset features. Returns {locate: {seq, ts, X}} and "names".
nb::dict features(const std::string& store, std::vector<std::uint16_t> targets,
                  std::vector<std::uint16_t> index, std::vector<std::uint32_t> sample_every) {
    if (sample_every.size() != targets.size())
        throw std::invalid_argument("one sample_every per target");
    std::vector<std::uint16_t> all = targets;
    all.insert(all.end(), index.begin(), index.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    auto slot = [&](std::uint16_t loc) {
        return static_cast<std::size_t>(std::lower_bound(all.begin(), all.end(), loc) -
                                        all.begin());
    };
    std::vector<std::size_t> index_slots;
    for (auto loc : index) index_slots.push_back(slot(loc));
    strategy::MultiFeatures mf(all.size(), index_slots);
    const std::size_t k = mf.count();

    struct Out {
        std::vector<std::uint64_t> seq, ts;
        std::vector<double> x;
        std::uint64_t n = 0;
        std::uint32_t every = 0;
        bool target = false;
    };
    std::vector<Out> out(all.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        out[slot(targets[i])].target = true;
        out[slot(targets[i])].every = std::max<std::uint32_t>(1, sample_every[i]);
    }
    {
        nb::gil_scoped_release release;
        data::MergedReader rd(store, all, 64);
        data::Record rec{};
        std::uint16_t loc;
        while (rd.next(rec, loc)) {
            const std::size_t s = slot(loc);
            if (!mf.on_itch(s, rec.data, rec.len, rec.seq)) continue;
            Out& o = out[s];
            if (!o.target || o.n++ % o.every) continue;
            o.seq.push_back(rec.seq);
            o.ts.push_back(mf.last_event().ts);
            const std::size_t at = o.x.size();
            o.x.resize(at + k);
            mf.row(s, mf.last_event().ts, o.x.data() + at);
        }
    }
    nb::dict d;
    for (std::size_t s = 0; s < all.size(); ++s) {
        if (!out[s].target) continue;
        Out& o = out[s];
        const std::size_t n = o.seq.size();
        nb::dict r;
        r["seq"] = array(std::move(o.seq), n);
        r["ts"] = array(std::move(o.ts), n);
        r["X"] = array(std::move(o.x), n, k);
        d[nb::int_(all[s])] = r;
    }
    d["names"] = feature_names(index.size());
    return d;
}

// Targets for every message of one symbol: {seq, ts, mid, Y}, mid in ticks of `tick`.
nb::dict labels(const std::string& store, std::uint16_t locate, std::vector<std::uint32_t> event_h,
                std::vector<std::uint64_t> clock_h_ns, std::uint32_t tick) {
    std::vector<std::uint64_t> seq, ts;
    std::vector<double> mid, y;
    strategy::Horizons h{std::move(event_h), std::move(clock_h_ns)};
    {
        nb::gil_scoped_release release;
        book::TickBook<> b(4096);
        book::ItchApply<book::TickBook<>> ap{b};
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        while (rd.next(rec)) {
            ap.seq = rec.seq;
            itch::dispatch(rec.data, rec.len, ap);
            const book::Bbo q = b.bbo();
            seq.push_back(rec.seq);
            ts.push_back(itch::detail::read_header(rec.data).timestamp);
            mid.push_back(q.bid_px && q.ask_px && q.ask_px > q.bid_px
                              ? (static_cast<double>(q.bid_px) + q.ask_px) / (2.0 * tick)
                              : std::numeric_limits<double>::quiet_NaN());
        }
        strategy::label(ts, mid, h, y);
    }
    const std::size_t n = seq.size();
    nb::dict d;
    d["seq"] = array(std::move(seq), n);
    d["ts"] = array(std::move(ts), n);
    d["mid"] = array(std::move(mid), n);
    d["Y"] = array(std::move(y), n, h.count());
    return d;
}

// Lifecycles of sampled real orders joining the best quote, and markouts of their fills.
nb::dict lifecycles(const std::string& store, std::uint16_t locate, std::uint64_t sample_one_in,
                    double horizon_s, std::vector<double> taus_s) {
    strategy::LifecycleTracker t(sample_one_in, horizon_s, taus_s);
    {
        nb::gil_scoped_release release;
        t.run(store, locate);
    }
    const auto& L = t.lifecycles();
    const std::size_t n = L.size();
    std::vector<std::uint64_t> ref(n), ts(n);
    std::vector<std::int8_t> side(n), outcome(n);
    std::vector<double> cov(n * 7), dur(n);
    for (std::size_t i = 0; i < n; ++i) {
        ref[i] = L[i].ref;
        ts[i] = L[i].ts;
        side[i] = L[i].side;
        outcome[i] = static_cast<std::int8_t>(L[i].outcome);
        dur[i] = L[i].duration_s;
        const double c[7] = {L[i].queue_ahead,
                             L[i].opposite_qty,
                             L[i].imbalance,
                             L[i].spread_ticks,
                             L[i].volatility,
                             L[i].signal,
                             static_cast<double>(L[i].shares)};
        std::copy(c, c + 7, cov.data() + i * 7);
    }
    const auto& F = t.fills();
    const std::size_t m = F.size(), k = taus_s.size();
    std::vector<std::uint64_t> fref(m), fts(m);
    std::vector<std::int8_t> fside(m), sweep(m), cancel_after(m);
    std::vector<double> fshares(m), fahead(m), marks(m * k);
    for (std::size_t i = 0; i < m; ++i) {
        fref[i] = F[i].ref;
        fts[i] = F[i].ts;
        fside[i] = F[i].side;
        sweep[i] = F[i].sweep;
        cancel_after[i] = F[i].cancel_after;
        fshares[i] = F[i].shares;
        fahead[i] = F[i].queue_ahead_at_arrival;
        std::copy(F[i].markout.begin(), F[i].markout.end(), marks.data() + i * k);
    }
    nb::dict d, f;
    d["ref"] = array(std::move(ref), n);
    d["ts"] = array(std::move(ts), n);
    d["side"] = array(std::move(side), n);
    d["outcome"] = array(std::move(outcome), n);
    d["duration_s"] = array(std::move(dur), n);
    d["covariates"] = array(std::move(cov), n, 7);
    f["ref"] = array(std::move(fref), m);
    f["ts"] = array(std::move(fts), m);
    f["side"] = array(std::move(fside), m);
    f["sweep"] = array(std::move(sweep), m);
    f["cancel_after"] = array(std::move(cancel_after), m);
    f["shares"] = array(std::move(fshares), m);
    f["queue_ahead"] = array(std::move(fahead), m);
    f["markout"] = array(std::move(marks), m, k);
    d["fills"] = f;
    return d;
}

// Regular grid of one symbol-day for the quoting MDP: per step the best quotes, the shares
// executed and cancelled at each starting best, whether it moved, the end mid and features.
nb::dict grid(const std::string& store, std::uint16_t target, std::vector<std::uint16_t> index,
              std::uint64_t step_ns, std::uint64_t start_ns, std::uint64_t end_ns) {
    strategy::GridSampler g(step_ns, start_ns, end_ns);
    {
        nb::gil_scoped_release release;
        g.run(store, target, std::move(index));
    }
    const auto& S = g.steps();
    const std::size_t n = S.size();
    std::vector<std::uint64_t> ts(n);
    std::vector<std::uint32_t> bid(n), ask(n);
    std::vector<double> q(n * 2), ex(n * 2), ca(n * 2), mid_end(n);
    std::vector<std::int8_t> moved(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = S[i].ts;
        bid[i] = S[i].bid_px;
        ask[i] = S[i].ask_px;
        q[2 * i] = S[i].bid_qty;
        q[2 * i + 1] = S[i].ask_qty;
        for (int k = 0; k < 2; ++k) {
            ex[2 * i + k] = S[i].exec[k];
            ca[2 * i + k] = S[i].cancel[k];
            moved[2 * i + k] = S[i].moved[k];
        }
        mid_end[i] = S[i].mid_end;
    }
    std::vector<double> rows = g.rows();
    nb::dict d;
    d["ts"] = array(std::move(ts), n);
    d["bid_px"] = array(std::move(bid), n);
    d["ask_px"] = array(std::move(ask), n);
    d["qty"] = array(std::move(q), n, 2);
    d["exec"] = array(std::move(ex), n, 2);
    d["cancel"] = array(std::move(ca), n, 2);
    d["moved"] = array(std::move(moved), n, 2);
    d["mid_end"] = array(std::move(mid_end), n);
    d["X"] = array(std::move(rows), n, g.features());
    return d;
}

double param(const std::map<std::string, double>& p, const char* k, double def) {
    const auto it = p.find(k);
    return it == p.end() ? def : it->second;
}

template <class S>
backtest::Summary run_one(backtest::Config c, S& s, const std::string& store, std::uint16_t target,
                          std::vector<std::uint16_t> index, std::uint32_t hysteresis) {
    backtest::Backtest<S> bt(c, s);
    bt.set_hysteresis(hysteresis);
    return bt.run(store, target, std::move(index));
}

// Runs one strategy on one symbol-day. cfg keys: tick, maker_rebate, taker_fee, fill_rule
// (0 queue, 1 trade-through), market_data_ns, order_entry_ns, processing_ns, start_ns, stop_ns,
// end_ns, sec_fee_per_million, taf_per_share, taf_max, max_position, max_order, collar_ticks.
nb::dict backtest_run(const std::string& store, std::uint16_t target,
                      std::vector<std::uint16_t> index, const std::string& name,
                      std::map<std::string, double> params, std::map<std::string, double> cfg) {
    backtest::Config c;
    c.exchange.tick = static_cast<std::uint32_t>(param(cfg, "tick", 100));
    c.exchange.maker_rebate = static_cast<std::int64_t>(param(cfg, "maker_rebate", 0));
    c.exchange.taker_fee = static_cast<std::int64_t>(param(cfg, "taker_fee", 0));
    c.fill_rule =
        param(cfg, "fill_rule", 0) ? engine::FillRule::TradeThrough : engine::FillRule::Queue;
    c.market_data_ns = static_cast<std::uint64_t>(param(cfg, "market_data_ns", 0));
    c.order_entry_ns = static_cast<std::uint64_t>(param(cfg, "order_entry_ns", 0));
    c.processing_ns = static_cast<std::uint64_t>(param(cfg, "processing_ns", 0));
    c.start_ns =
        static_cast<std::uint64_t>(param(cfg, "start_ns", static_cast<double>(c.start_ns)));
    c.stop_ns = static_cast<std::uint64_t>(param(cfg, "stop_ns", static_cast<double>(c.stop_ns)));
    c.end_ns = static_cast<std::uint64_t>(param(cfg, "end_ns", static_cast<double>(c.end_ns)));
    c.sec_fee_per_million = static_cast<std::int64_t>(param(cfg, "sec_fee_per_million", 0));
    c.taf_per_share = static_cast<std::int64_t>(param(cfg, "taf_per_share", 0));
    c.taf_max = static_cast<std::int64_t>(param(cfg, "taf_max", 0));
    c.risk.max_position = static_cast<std::int64_t>(param(cfg, "max_position", 1000));
    c.risk.max_order = static_cast<std::uint32_t>(param(cfg, "max_order", 500));
    c.risk.collar_ticks = static_cast<std::uint32_t>(param(cfg, "collar_ticks", 20));
    const auto size = static_cast<std::uint32_t>(param(params, "size", 100));
    const auto max_inv = static_cast<std::int64_t>(param(params, "max_inventory", 500));
    const auto hysteresis = static_cast<std::uint32_t>(param(params, "hysteresis_ticks", 0));

    backtest::Summary r;
    {
        nb::gil_scoped_release release;
        if (name == "zero") {
            backtest::Zero s;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name == "naive") {
            backtest::NaiveJoin s{size, max_inv};
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name == "random_taker") {
            backtest::RandomTaker s;
            s.rate = param(params, "rate", 0.001);
            s.size = size;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name == "random_passive") {
            backtest::RandomPassive s;
            s.size = size;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name == "perfect_foresight") {
            backtest::PerfectForesight s;
            // Mid series from the labeler path: the strategy alone may read the future.
            book::TickBook<> b(4096);
            book::ItchApply<book::TickBook<>> ap{b};
            data::SymbolReader rd(store, target);
            data::Record rec{};
            while (rd.next(rec)) {
                itch::dispatch(rec.data, rec.len, ap);
                const book::Bbo q = b.bbo();
                if (!q.bid_px || !q.ask_px || q.ask_px <= q.bid_px) continue;
                s.ts.push_back(itch::detail::read_header(rec.data).timestamp);
                s.mid.push_back((static_cast<double>(q.bid_px) + q.ask_px) /
                                (2.0 * c.exchange.tick));
            }
            s.horizon_ns = static_cast<std::uint64_t>(param(params, "horizon_ns", 1e9));
            s.cost_ticks = param(params, "cost_ticks", 0.3);
            s.size = size;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name == "avellaneda_stoikov") {
            backtest::AvellanedaStoikov s;
            s.gamma = param(params, "gamma", 0.1);
            s.A = param(params, "A", 1.0);
            s.k = param(params, "k", 1.5);
            s.glft = param(params, "glft", 1) != 0;
            s.size = size;
            s.max_inventory = max_inv;
            s.end_ns = c.end_ns;
            s.tick = c.exchange.tick;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name.starts_with("dp:")) {
            backtest::DpPolicy s;
            s.load(name.substr(3));
            s.tick = c.exchange.tick;
            r = run_one(c, s, store, target, index, hysteresis);
        } else if (name.starts_with("ext:")) {
            backtest::Extended s;
            s.dp.load(name.substr(4));
            s.dp.tick = c.exchange.tick;
            s.toxicity = param(params, "toxicity", 1) != 0;
            s.taking = param(params, "taking", 1) != 0;
            s.vol_limit = param(params, "vol_limit", 1.0);
            s.fee_ticks = static_cast<double>(c.exchange.taker_fee) / (c.exchange.tick * 100.0);
            s.take_margin = param(params, "take_margin", 0.1);
            r = run_one(c, s, store, target, index, hysteresis);
        } else {
            throw std::invalid_argument("unknown strategy " + name);
        }
    }
    nb::dict d;
    d["total"] = r.total;
    d["spread"] = r.spread;
    d["inventory_pnl"] = r.inventory_pnl;
    d["fees"] = r.fees;
    d["adverse"] = r.adverse;
    d["end_inventory"] = r.end_inventory;
    d["max_abs_inventory"] = r.max_abs_inventory;
    d["volume"] = r.volume;
    d["fills"] = r.fills;
    d["orders"] = r.orders;
    d["cancels"] = r.cancels;
    d["rejects"] = r.rejects;
    d["events"] = r.events;
    d["fills_during_cancel"] = r.fills_during_cancel;
    d["diverted"] = r.divergence.diverted;
    d["taken"] = r.divergence.taken;
    d["through_fills"] = r.divergence.through_fills;
    d["hidden_fills"] = r.divergence.hidden_fills;
    const std::size_t n = r.fill_ts.size(), mins = r.minute_pnl.size();
    d["minute_pnl"] = array(std::move(r.minute_pnl), mins);
    d["fill_ts"] = array(std::move(r.fill_ts), n);
    d["fill_signed_shares"] = array(std::move(r.fill_signed_shares), n);
    d["fill_price"] = array(std::move(r.fill_price), n);
    d["fill_maker"] = array(std::move(r.fill_maker), n);
    return d;
}

// Queue-reactive calibration record of one symbol (strategy/qr_events.hpp).
nb::dict qr_events(const std::string& store, std::uint16_t locate, int K, std::uint32_t tick,
                   std::uint64_t start_ns, std::uint64_t end_ns) {
    strategy::QrRecorder rec(K, tick, start_ns, end_ns);
    {
        nb::gil_scoped_release release;
        rec.run(store, locate);
    }
    strategy::QrRecord r = rec.record();
    const std::size_t n = r.ts.size(), m = r.moves_ts.size();
    nb::dict d;
    d["ts"] = array(std::move(r.ts), n);
    d["kind"] = array(std::move(r.kind), n);
    d["side"] = array(std::move(r.side), n);
    d["level"] = array(std::move(r.level), n);
    d["shares"] = array(std::move(r.shares), n);
    d["after_move"] = array(std::move(r.after_move), n);
    d["q"] = array(std::move(r.q), n, static_cast<std::size_t>(2 * K));
    d["moves_ts"] = array(std::move(r.moves_ts), m);
    d["moves_dir"] = array(std::move(r.moves_dir), m);
    d["episodes_moved"] = r.episodes_moved;
    d["episodes_refilled"] = r.episodes_refilled;
    return d;
}

// Simulates one session of the queue-reactive model into a BinaryFILE stream at `path`.
// Rate tables are K x (N + 1) arrays; returns the event and move counts.
nb::dict qr_simulate(int K, int N, std::uint32_t aes, std::uint32_t p_ref, double theta,
                     std::vector<double> L, std::vector<double> C, std::vector<double> M,
                     std::vector<double> init, std::uint64_t start_ns, std::uint64_t end_ns,
                     std::uint64_t seed, const std::string& path, std::uint32_t tick,
                     std::vector<double> size_L, std::vector<double> size_M) {
    sources::QrParams p;
    p.K = K, p.N = N, p.aes = aes, p.tick = tick, p.p_ref = p_ref, p.theta = theta;
    p.L = std::move(L), p.C = std::move(C), p.M = std::move(M), p.init = std::move(init);
    p.size_L = std::move(size_L), p.size_M = std::move(size_M);
    p.start_ns = start_ns, p.end_ns = end_ns;
    sources::QueueReactive sim(std::move(p), seed);
    std::vector<std::uint8_t> raw;
    {
        nb::gil_scoped_release release;
        sim.day(raw);
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(raw.data()),
                                                 static_cast<std::streamsize>(raw.size()));
    nb::dict d;
    d["events"] = sim.events();
    d["moves"] = sim.moves();
    d["bytes"] = raw.size();
    return d;
}

// Signed trades and executions of one symbol (strategy/trades.hpp).
nb::dict trades(const std::string& store, std::uint16_t locate, std::uint64_t start_ns,
                std::uint64_t end_ns) {
    strategy::TradeRecorder rec(start_ns, end_ns);
    {
        nb::gil_scoped_release release;
        rec.run(store, locate);
    }
    strategy::TradeRecord r = rec.record();
    const std::size_t n = r.ts.size(), m = r.exec_ts.size();
    nb::dict d, e;
    d["ts"] = array(std::move(r.ts), n);
    d["sign"] = array(std::move(r.sign), n);
    d["shares"] = array(std::move(r.shares), n);
    d["notional"] = array(std::move(r.notional), n);
    d["mid_before"] = array(std::move(r.mid_before), n);
    d["mid_after"] = array(std::move(r.mid_after), n);
    e["ts"] = array(std::move(r.exec_ts), m);
    e["sign"] = array(std::move(r.exec_sign), m);
    e["shares"] = array(std::move(r.exec_shares), m);
    e["mpid"] = array(std::move(r.exec_mpid), m);
    e["hidden"] = array(std::move(r.exec_hidden), m);
    d["executions"] = e;
    return d;
}

// Event tokens of one symbol for the event transformer (strategy/event_tokens.hpp).
nb::dict tokens(const std::string& store, std::uint16_t locate, std::uint64_t start_ns,
                std::uint64_t end_ns) {
    strategy::Tokenizer t(start_ns, end_ns);
    {
        nb::gil_scoped_release release;
        t.run(store, locate);
    }
    strategy::TokenRecord r = t.record();
    const std::size_t n = r.ts.size();
    nb::dict d;
    d["ts"] = array(std::move(r.ts), n);
    d["type"] = array(std::move(r.type), n);
    d["side"] = array(std::move(r.side), n);
    d["dist"] = array(std::move(r.dist), n);
    d["size"] = array(std::move(r.size), n);
    d["log_dt"] = array(std::move(r.log_dt), n);
    d["mid_after"] = array(std::move(r.mid_after), n);
    return d;
}

// Runs the C++ event transformer over one token sequence; returns forecast and generator logits.
using U8 = nb::ndarray<const std::uint8_t, nb::ndim<1>, nb::c_contig>;
nb::dict transformer_run(const std::string& weights, U8 type, U8 side, U8 dist, U8 size,
                         nb::ndarray<const float, nb::ndim<1>, nb::c_contig> log_dt, bool avx2) {
    const strategy::EventTransformer m(weights);
    const std::size_t n = type.shape(0);
    const auto nf = static_cast<std::size_t>(m.forecasts());
    std::vector<float> f(n * nf), g(n * strategy::EventTransformer::kGen);
    {
        nb::gil_scoped_release release;
        auto s = m.state();
        const std::uint8_t *ty = type.data(), *si = side.data(), *di = dist.data(), *sz = size.data();
        const float* dt = log_dt.data();
        for (std::size_t i = 0; i < n; ++i) {
            const strategy::EventToken t{ty[i], si[i], di[i], sz[i], dt[i]};
            if (avx2)
                m.step<true>(s, t, f.data() + i * nf, g.data() + i * strategy::EventTransformer::kGen);
            else
                m.step<false>(s, t, f.data() + i * nf, g.data() + i * strategy::EventTransformer::kGen);
        }
    }
    nb::dict d;
    d["forecast"] = array(std::move(f), n, nf);
    d["gen"] = array(std::move(g), n, static_cast<std::size_t>(strategy::EventTransformer::kGen));
    return d;
}

// L2 snapshots on the tick grid: at each query time (sorted), the best prices and the shares
// at the L prices from each best outwards (empty prices included).
nb::dict snapshots(const std::string& store, std::uint16_t locate, std::vector<std::uint64_t> at,
                   int L, std::uint32_t tick) {
    const std::size_t n = at.size();
    std::vector<std::uint32_t> bid(n), ask(n);
    std::vector<std::uint64_t> bq(n * static_cast<std::size_t>(L)), aq(n * static_cast<std::size_t>(L));
    {
        nb::gil_scoped_release release;
        book::TickBook<> b;
        book::ItchApply<book::TickBook<>> ap{b};
        data::SymbolReader rd(store, locate);
        data::Record rec{};
        std::size_t k = 0;
        auto take = [&] {
            const book::Bbo q = b.bbo();
            bid[k] = q.bid_px, ask[k] = q.ask_px;
            for (int j = 0; j < L; ++j) {
                const auto u = static_cast<std::uint32_t>(j) * tick;
                const auto o = k * static_cast<std::size_t>(L) + static_cast<std::size_t>(j);
                bq[o] = q.bid_px > u ? b.level_qty(book::Side::Buy, q.bid_px - u) : 0;
                aq[o] = q.ask_px ? b.level_qty(book::Side::Sell, q.ask_px + u) : 0;
            }
            ++k;
        };
        while (k < n && rd.next(rec)) {
            const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
            while (k < n && ts > at[k]) take();
            itch::dispatch(rec.data, rec.len, ap);
        }
        while (k < n) take();
    }
    nb::dict d;
    d["bid_px"] = array(std::move(bid), n);
    d["ask_px"] = array(std::move(ask), n);
    d["bid_qty"] = array(std::move(bq), n, static_cast<std::size_t>(L));
    d["ask_qty"] = array(std::move(aq), n, static_cast<std::size_t>(L));
    return d;
}

}  // namespace

NB_MODULE(hftpy, m) {
    m.def("symbols", &symbols, nb::arg("store"));
    m.def("features", &features, nb::arg("store"), nb::arg("targets"), nb::arg("index"),
          nb::arg("sample_every"));
    m.def("labels", &labels, nb::arg("store"), nb::arg("locate"), nb::arg("event_h"),
          nb::arg("clock_h_ns"), nb::arg("tick") = 100);
    m.def("lifecycles", &lifecycles, nb::arg("store"), nb::arg("locate"), nb::arg("sample_one_in"),
          nb::arg("horizon_s"), nb::arg("taus_s"));
    m.def("grid", &grid, nb::arg("store"), nb::arg("target"), nb::arg("index"), nb::arg("step_ns"),
          nb::arg("start_ns"), nb::arg("end_ns"));
    m.def("backtest", &backtest_run, nb::arg("store"), nb::arg("target"), nb::arg("index"),
          nb::arg("strategy"), nb::arg("params"), nb::arg("config"));
    m.def("transformer_run", &transformer_run, nb::arg("weights"), nb::arg("type"), nb::arg("side"),
          nb::arg("dist"), nb::arg("size"), nb::arg("log_dt"), nb::arg("avx2") = true);
    m.def("snapshots", &snapshots, nb::arg("store"), nb::arg("locate"), nb::arg("at"), nb::arg("L") = 10,
          nb::arg("tick") = 100);
    m.def("tokens", &tokens, nb::arg("store"), nb::arg("locate"), nb::arg("start_ns"), nb::arg("end_ns"));
    m.def("trades", &trades, nb::arg("store"), nb::arg("locate"), nb::arg("start_ns"), nb::arg("end_ns"));
    m.def("qr_events", &qr_events, nb::arg("store"), nb::arg("locate"), nb::arg("K"),
          nb::arg("tick"), nb::arg("start_ns"), nb::arg("end_ns"));
    m.def("qr_simulate", &qr_simulate, nb::arg("K"), nb::arg("N"), nb::arg("aes"), nb::arg("p_ref"),
          nb::arg("theta"), nb::arg("L"), nb::arg("C"), nb::arg("M"), nb::arg("init"),
          nb::arg("start_ns"), nb::arg("end_ns"), nb::arg("seed"), nb::arg("path"), nb::arg("tick") = 100,
          nb::arg("size_L") = std::vector<double>{}, nb::arg("size_M") = std::vector<double>{});
    m.attr("lifecycle_covariates") =
        nb::make_tuple("queue_ahead", "opposite_qty", "imbalance", "spread_ticks", "volatility",
                       "ofi_signal", "shares");
}
