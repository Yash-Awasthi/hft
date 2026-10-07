// Python module for research: features and labels computed by the same C++ code the
// backtest runs. Features and labels are separate calls; join them on `seq`.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "backtest/strategies.hpp"
#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"
#include "strategy/labeler.hpp"
#include "strategy/lifecycles.hpp"
#include "strategy/multi_features.hpp"

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

}  // namespace

NB_MODULE(hftpy, m) {
    m.def("symbols", &symbols, nb::arg("store"));
    m.def("features", &features, nb::arg("store"), nb::arg("targets"), nb::arg("index"),
          nb::arg("sample_every"));
    m.def("labels", &labels, nb::arg("store"), nb::arg("locate"), nb::arg("event_h"),
          nb::arg("clock_h_ns"), nb::arg("tick") = 100);
    m.def("lifecycles", &lifecycles, nb::arg("store"), nb::arg("locate"), nb::arg("sample_one_in"),
          nb::arg("horizon_s"), nb::arg("taus_s"));
    m.def("backtest", &backtest_run, nb::arg("store"), nb::arg("target"), nb::arg("index"),
          nb::arg("strategy"), nb::arg("params"), nb::arg("config"));
    m.attr("lifecycle_covariates") =
        nb::make_tuple("queue_ahead", "opposite_qty", "imbalance", "spread_ticks", "volatility",
                       "ofi_signal", "shares");
}
