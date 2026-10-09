// Runs one strategy on one symbol-day of an ITCH store through the event-driven backtest
// (latencies, virtual orders in the real queues, fees, risk, PnL identity every event) and
// prints a summary.
//
//   itch_backtest <store-dir> <symbol|locate> <strategy> [key=value ...] [--fills FILE]
//                 [--minutes FILE] [--index LOC,LOC,...]
//
// Strategies: zero, naive, random_taker, random_passive, perfect_foresight,
// avellaneda_stoikov, dp:<policy.bin>, ext:<policy.bin>.
// Strategy keys: size, max_inventory, hysteresis_ticks, rate, horizon_ns, cost_ticks, gamma,
//   A, k, glft, online, max_dist_ticks, online_tau_s, online_prior_s, toxicity, taking,
//   vol_limit, take_margin.
// Venue keys: tick, maker_rebate, taker_fee (micro-dollars per share), fill_rule (0 queue,
//   1 trade-through), market_data_ns, order_entry_ns, processing_ns, start_ns, stop_ns, end_ns,
//   sec_fee_per_million, taf_per_share, taf_max, max_position, max_order, collar_ticks.
// Money is printed in dollars; the accounting itself is in integer micro-dollars.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "backtest/strategies.hpp"
#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

using namespace hft;

namespace {

using Params = std::map<std::string, double>;

double get(const Params& p, const char* k, double def) {
    const auto it = p.find(k);
    return it == p.end() ? def : it->second;
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

// Locate of a ticker: the stock directory message is among the first records of a symbol.
std::uint16_t find_locate(const std::filesystem::path& store, const std::string& sym) {
    if (!sym.empty() && std::all_of(sym.begin(), sym.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return static_cast<std::uint16_t>(std::stoul(sym));
    for (const auto& e : std::filesystem::directory_iterator(store)) {
        if (e.path().extension() != ".idx") continue;
        const auto loc = static_cast<std::uint16_t>(std::stoul(e.path().stem().string()));
        data::SymbolReader rd(store, loc);
        data::Record rec{};
        std::string name;
        Name h{name};
        for (int i = 0; i < 8 && name.empty() && rd.next(rec); ++i)
            if (rec.data[0] == 'R') itch::dispatch(rec.data, rec.len, h);
        if (name == sym) return loc;
    }
    throw std::runtime_error("symbol " + sym + " not in store");
}

template <class S>
backtest::Summary run(const backtest::Config& c, S& s, const std::string& store, std::uint16_t target,
                      std::vector<std::uint16_t> index, std::uint32_t hysteresis) {
    backtest::Backtest<S> bt(c, s);
    bt.set_hysteresis(hysteresis);
    return bt.run(store, target, std::move(index));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: itch_backtest <store-dir> <symbol|locate> <strategy> [key=value ...] "
                     "[--fills FILE] [--minutes FILE] [--index LOC,...]\n");
        return 2;
    }
    const std::string store = argv[1], strategy = argv[3];
    Params p;
    std::string fills_path, minutes_path;
    std::vector<std::uint16_t> index;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--fills" || a == "--minutes" || a == "--index") && i + 1 < argc) {
            const std::string v = argv[++i];
            if (a == "--fills") fills_path = v;
            else if (a == "--minutes") minutes_path = v;
            else
                for (std::size_t at = 0; at < v.size();) {
                    const std::size_t comma = std::min(v.find(',', at), v.size());
                    index.push_back(static_cast<std::uint16_t>(std::stoul(v.substr(at, comma - at))));
                    at = comma + 1;
                }
            continue;
        }
        const std::size_t eq = a.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "expected key=value, got %s\n", a.c_str());
            return 2;
        }
        p[a.substr(0, eq)] = std::atof(a.c_str() + eq + 1);
    }

    backtest::Config c;
    c.exchange.tick = static_cast<std::uint32_t>(get(p, "tick", 100));
    c.exchange.maker_rebate = static_cast<std::int64_t>(get(p, "maker_rebate", 0));
    c.exchange.taker_fee = static_cast<std::int64_t>(get(p, "taker_fee", 0));
    c.fill_rule = get(p, "fill_rule", 0) != 0 ? engine::FillRule::TradeThrough : engine::FillRule::Queue;
    c.market_data_ns = static_cast<std::uint64_t>(get(p, "market_data_ns", 0));
    c.order_entry_ns = static_cast<std::uint64_t>(get(p, "order_entry_ns", 0));
    c.processing_ns = static_cast<std::uint64_t>(get(p, "processing_ns", 0));
    c.start_ns = static_cast<std::uint64_t>(get(p, "start_ns", static_cast<double>(c.start_ns)));
    c.stop_ns = static_cast<std::uint64_t>(get(p, "stop_ns", static_cast<double>(c.stop_ns)));
    c.end_ns = static_cast<std::uint64_t>(get(p, "end_ns", static_cast<double>(c.end_ns)));
    c.sec_fee_per_million = static_cast<std::int64_t>(get(p, "sec_fee_per_million", 0));
    c.taf_per_share = static_cast<std::int64_t>(get(p, "taf_per_share", 0));
    c.taf_max = static_cast<std::int64_t>(get(p, "taf_max", 0));
    c.risk.max_position = static_cast<std::int64_t>(get(p, "max_position", 1000));
    c.risk.max_order = static_cast<std::uint32_t>(get(p, "max_order", 500));
    c.risk.collar_ticks = static_cast<std::uint32_t>(get(p, "collar_ticks", 20));
    const auto size = static_cast<std::uint32_t>(get(p, "size", 100));
    const auto max_inv = static_cast<std::int64_t>(get(p, "max_inventory", 500));
    const auto hyst = static_cast<std::uint32_t>(get(p, "hysteresis_ticks", 0));

    try {
        const std::uint16_t target = find_locate(store, argv[2]);
        const auto t0 = std::chrono::steady_clock::now();
        backtest::Summary r;
        if (strategy == "zero") {
            backtest::Zero s;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy == "naive") {
            backtest::NaiveJoin s{size, max_inv};
            r = run(c, s, store, target, index, hyst);
        } else if (strategy == "random_taker") {
            backtest::RandomTaker s;
            s.rate = get(p, "rate", 0.001);
            s.size = size;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy == "random_passive") {
            backtest::RandomPassive s;
            s.size = size;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy == "perfect_foresight") {
            backtest::PerfectForesight s;
            // The mid series is read ahead: this strategy alone may see the future.
            book::TickBook b(4096);
            book::ItchApply<book::TickBook> ap{b};
            data::SymbolReader rd(store, target);
            data::Record rec{};
            while (rd.next(rec)) {
                itch::dispatch(rec.data, rec.len, ap);
                const book::Bbo q = b.bbo();
                if (!q.bid_px || !q.ask_px || q.ask_px <= q.bid_px) continue;
                s.ts.push_back(itch::detail::read_header(rec.data).timestamp);
                s.mid.push_back((static_cast<double>(q.bid_px) + q.ask_px) / (2.0 * c.exchange.tick));
            }
            s.horizon_ns = static_cast<std::uint64_t>(get(p, "horizon_ns", 1e9));
            s.cost_ticks = get(p, "cost_ticks", 0.3);
            s.size = size;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy == "avellaneda_stoikov") {
            backtest::AvellanedaStoikov s;
            s.gamma = get(p, "gamma", 0.1);
            s.A = get(p, "A", 1.0);
            s.k = get(p, "k", 1.5);
            s.glft = get(p, "glft", 1) != 0;
            s.online = get(p, "online", 0) != 0;
            s.max_dist_ticks = get(p, "max_dist_ticks", 0);
            s.online_tau_s = get(p, "online_tau_s", 600);
            s.online_prior_s = get(p, "online_prior_s", 60);
            s.size = size;
            s.max_inventory = max_inv;
            s.end_ns = c.end_ns;
            s.tick = c.exchange.tick;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy.starts_with("dp:")) {
            backtest::DpPolicy s;
            s.load(strategy.substr(3));
            s.tick = c.exchange.tick;
            r = run(c, s, store, target, index, hyst);
        } else if (strategy.starts_with("ext:")) {
            backtest::Extended s;
            s.dp.load(strategy.substr(4));
            s.dp.tick = c.exchange.tick;
            s.toxicity = get(p, "toxicity", 1) != 0;
            s.taking = get(p, "taking", 1) != 0;
            s.vol_limit = get(p, "vol_limit", 1.0);
            s.fee_ticks = static_cast<double>(c.exchange.taker_fee) / (c.exchange.tick * 100.0);
            s.take_margin = get(p, "take_margin", 0.1);
            r = run(c, s, store, target, index, hyst);
        } else {
            std::fprintf(stderr, "unknown strategy %s\n", strategy.c_str());
            return 2;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        auto usd = [](std::int64_t micro) { return static_cast<double>(micro) * 1e-6; };
        std::printf("symbol\t%s\nlocate\t%u\nstrategy\t%s\n", argv[2], target, strategy.c_str());
        std::printf("total_usd\t%.2f\nspread_usd\t%.2f\ninventory_usd\t%.2f\nfees_usd\t%.2f\nadverse_usd\t%.2f\n",
                    usd(r.total), usd(r.spread), usd(r.inventory_pnl), usd(r.fees), usd(r.adverse));
        std::printf("fills\t%llu\norders\t%llu\ncancels\t%llu\nrejects\t%llu\nvolume\t%lld\n",
                    (unsigned long long)r.fills, (unsigned long long)r.orders, (unsigned long long)r.cancels,
                    (unsigned long long)r.rejects, (long long)r.volume);
        std::printf("end_inventory\t%lld\nmax_abs_inventory\t%lld\nfills_during_cancel\t%llu\n",
                    (long long)r.end_inventory, (long long)r.max_abs_inventory, (unsigned long long)r.fills_during_cancel);
        std::printf("through_fills\t%llu\nhidden_fills\t%llu\nevents\t%llu\nseconds\t%.2f\n",
                    (unsigned long long)r.divergence.through_fills, (unsigned long long)r.divergence.hidden_fills,
                    (unsigned long long)r.events, secs);
        if (!fills_path.empty()) {
            std::FILE* f = std::fopen(fills_path.c_str(), "w");
            if (!f) throw std::runtime_error("cannot write " + fills_path);
            std::fprintf(f, "ts_ns\tsigned_shares\tprice\tmaker\n");
            for (std::size_t i = 0; i < r.fill_ts.size(); ++i)
                std::fprintf(f, "%llu\t%lld\t%u\t%u\n", (unsigned long long)r.fill_ts[i],
                             (long long)r.fill_signed_shares[i], r.fill_price[i], r.fill_maker[i]);
            std::fclose(f);
        }
        if (!minutes_path.empty()) {
            std::FILE* f = std::fopen(minutes_path.c_str(), "w");
            if (!f) throw std::runtime_error("cannot write " + minutes_path);
            std::fprintf(f, "minute\ttotal_usd\n");
            for (std::size_t i = 0; i < r.minute_pnl.size(); ++i) std::fprintf(f, "%zu\t%.2f\n", i, usd(r.minute_pnl[i]));
            std::fclose(f);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
