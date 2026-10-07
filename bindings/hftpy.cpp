// Python module for research: features and labels computed by the same C++ code the
// backtest runs. Features and labels are separate calls; join them on `seq`.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"
#include "strategy/labeler.hpp"
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
            l.append(nb::str(("index" + std::to_string(k) + "_mom_" + std::to_string(int(tau * 1000)) + "ms").c_str()));
    return l;
}

// Features of each target symbol at every sample_every-th event of that symbol, with the
// index symbols' cross-asset features. Returns {locate: {seq, ts, X}} and "names".
nb::dict features(const std::string& store, std::vector<std::uint16_t> targets,
                  std::vector<std::uint16_t> index, std::vector<std::uint32_t> sample_every) {
    if (sample_every.size() != targets.size()) throw std::invalid_argument("one sample_every per target");
    std::vector<std::uint16_t> all = targets;
    all.insert(all.end(), index.begin(), index.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    auto slot = [&](std::uint16_t loc) {
        return static_cast<std::size_t>(std::lower_bound(all.begin(), all.end(), loc) - all.begin());
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

}  // namespace

NB_MODULE(hftpy, m) {
    m.def("symbols", &symbols, nb::arg("store"));
    m.def("features", &features, nb::arg("store"), nb::arg("targets"), nb::arg("index"),
          nb::arg("sample_every"));
    m.def("labels", &labels, nb::arg("store"), nb::arg("locate"), nb::arg("event_h"),
          nb::arg("clock_h_ns"), nb::arg("tick") = 100);
}
