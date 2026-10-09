// Queue-tracking check of DESIGN.md section 2 on real data. A sample of real orders is taken
// out of the replayed book and re-inserted as virtual orders at their own queue positions;
// each must receive exactly the executions the data gives the real order, at the same
// messages. Prints one line per symbol and a total.
// Usage: reinsert <store> <queue|trade_through> <sample-1-in-n> <locate>...

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/store.hpp"
#include "engine/replay_exchange.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft;

struct Tally {
    std::uint64_t mirrored = 0, executed = 0, exact = 0, exec_shares = 0, fill_shares = 0;
    std::uint64_t early = 0, missed = 0;  // orders filled before / not at the data's executions
    // Fill shares not matched by the data, by what triggered them.
    std::uint64_t by_hidden = 0, by_through = 0, by_behind = 0;
    // Executions of a mirrored order while its copy still had shares queued ahead: each one
    // is a queue-tracking error, whatever the fill rule.
    std::uint64_t own_execs = 0, ahead_nonzero = 0;
    // L2-only estimate of shares ahead (executions from the front, cancels pro rata) against
    // the exact count, at every message that changes the order's level.
    double l2_abs_err = 0, l2_ahead_sum = 0;
    std::uint64_t l2_samples = 0;
    void add(const Tally& o) {
        mirrored += o.mirrored;
        executed += o.executed;
        exact += o.exact;
        exec_shares += o.exec_shares;
        fill_shares += o.fill_shares;
        early += o.early;
        missed += o.missed;
        by_hidden += o.by_hidden;
        by_through += o.by_through;
        by_behind += o.by_behind;
        own_execs += o.own_execs;
        ahead_nonzero += o.ahead_nonzero;
        l2_abs_err += o.l2_abs_err;
        l2_ahead_sum += o.l2_ahead_sum;
        l2_samples += o.l2_samples;
    }
};

// Deterministic sample of references, independent of the order of arrival.
bool sampled(std::uint64_t ref, std::uint64_t n) {
    return (ref * 0x9E3779B97F4A7C15ull >> 40) % n == 0;
}

Tally run(const char* store, std::uint16_t loc, engine::FillRule rule, std::uint64_t n,
          std::string& symbol) {
    engine::ReplayExchange<> x({}, rule, 1 << 16);
    using Seq = std::vector<std::pair<std::uint64_t, std::uint32_t>>;  // (feed seq, shares)
    std::unordered_map<std::uint64_t, Seq> data, fills;                // keyed by real ref
    std::unordered_map<std::uint64_t, std::uint64_t> real_of;          // virtual -> real
    // Live mirrored orders by (side, price): (virtual reference, L2-only estimate of shares ahead).
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint64_t, double>>> l2_est;
    auto level_key = [](book::Side s, std::uint32_t px) {
        return std::uint64_t{px} << 1 | static_cast<std::uint64_t>(s);
    };
    data::SymbolReader rd(store, loc);
    data::Record rec{};
    std::uint64_t seq = 0;
    Tally t;
    char cause = 0;  // 'H' hidden print, 'T' trade through our price, 'B' order behind us, 'S' self
    std::uint32_t exec_px = 0;
    auto sink = [&](const engine::Event& e) {
        if (e.type != engine::EventType::Fill) return;
        const auto it = real_of.find(e.ref);
        if (it == real_of.end()) return;
        fills[it->second].push_back({seq, e.qty});
        if (cause == 'H') t.by_hidden += e.qty;
        if (cause == 'O') (exec_px == e.price ? t.by_behind : t.by_through) += e.qty;
    };
    while (rd.next(rec)) {
        seq = rec.seq;
        const char type = static_cast<char>(rec.data[0]);
        if (type == 'R') {
            symbol.assign(reinterpret_cast<const char*>(rec.data + 11), 8);
            symbol.erase(symbol.find_last_not_of(' ') + 1);
        }
        if ((type == 'A' || type == 'F') && sampled(load_be64(rec.data + 11), n)) {
            x.mirror(load_be64(rec.data + 11), 1);
            ++t.mirrored;
        }
        cause = type == 'P' ? 'H' : 0;
        if (type == 'E' || type == 'C') {
            const std::uint64_t ref = load_be64(rec.data + 11);
            if (const std::uint64_t v = x.mirrored(ref)) {
                data[ref].push_back({seq, load_be32(rec.data + 19)});
                cause = 'S';
                ++t.own_execs;
                t.ahead_nonzero += x.book().order(v) && x.queue_ahead(v) != 0;
            } else if (const auto o = x.book().order(ref)) {
                cause = 'O';
                exec_px = o->price;
            }
        }
        // L2 view: a real order at a mirrored order's level loses shares.
        std::uint32_t l2_px = 0, l2_q = 0;
        book::Side l2_side = book::Side::Buy;
        bool l2_trade = false;
        if (type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U') {
            const std::uint64_t ref = load_be64(rec.data + 11);
            if (const auto o = x.book().order(ref); o && !x.mirrored(ref)) {
                l2_px = o->price;
                l2_side = o->side;
                l2_trade = type == 'E' || type == 'C';
                l2_q =
                    type == 'X' || l2_trade ? std::min(load_be32(rec.data + 19), o->qty) : o->qty;
            }
        }
        const std::uint64_t level_before = l2_q ? x.book().level_qty(l2_side, l2_px) : 0;
        x.on_itch(rec.data, rec.len, sink);
        if (l2_q) {
            auto& at = l2_est[level_key(l2_side, l2_px)];
            std::erase_if(at, [&](const auto& p) { return !x.book().order(p.first); });
            for (auto& [v, est] : at) {
                const auto o = x.book().order(v);
                if (l2_trade)
                    est -= std::min<double>(est, l2_q);
                else
                    est -= l2_q * est /
                           std::max<double>(1, static_cast<double>(level_before) - o->qty);
                const double exact = static_cast<double>(x.queue_ahead(v));
                t.l2_abs_err += std::fabs(est - exact);
                t.l2_ahead_sum += exact;
                ++t.l2_samples;
            }
        }
        if (type == 'A' || type == 'F') {
            const std::uint64_t ref = load_be64(rec.data + 11);
            if (const std::uint64_t v = x.mirrored(ref)) {
                real_of[v] = ref;
                const auto o = x.book().order(v);
                l2_est[level_key(o->side, o->price)].push_back(
                    {v, static_cast<double>(x.queue_ahead(v))});
            }
        }
    }
    for (const auto& [v, real] : real_of) {
        const Seq& d = data[real];
        const Seq& f = fills[real];
        if (!d.empty()) ++t.executed;
        for (const auto& [s, q] : d) t.exec_shares += q;
        for (const auto& [s, q] : f) t.fill_shares += q;
        if (d == f) {
            t.exact += !d.empty();
            continue;
        }
        if (!f.empty() && (d.empty() || f.front().first < d.front().first))
            ++t.early;
        else
            ++t.missed;
    }
    return t;
}

void print(const char* name, const Tally& t) {
    const std::uint64_t v[] = {t.mirrored,   t.executed,    t.exact,       t.early,
                               t.missed,     t.exec_shares, t.fill_shares, t.by_hidden,
                               t.by_through, t.by_behind,   t.own_execs,   t.ahead_nonzero};
    std::printf("%s", name);
    for (std::uint64_t x : v) std::printf(" %" PRIu64, x);
    std::printf(" %.1f %.1f\n", t.l2_samples ? t.l2_abs_err / t.l2_samples : 0.0,
                t.l2_samples ? t.l2_ahead_sum / t.l2_samples : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: reinsert <store> <queue|trade_through> <1-in-n> <locate>...\n");
        return 2;
    }
    const engine::FillRule rule = std::strcmp(argv[2], "queue") == 0
                                      ? engine::FillRule::Queue
                                      : engine::FillRule::TradeThrough;
    const std::uint64_t n = std::strtoull(argv[3], nullptr, 10);
    std::printf(
        "symbol mirrored executed exact early missed exec_shares fill_shares "
        "unmatched_by_hidden unmatched_by_through unmatched_by_behind own_execs ahead_nonzero "
        "l2_mean_abs_error l2_mean_exact_ahead\n");
    Tally all;
    for (int i = 4; i < argc; ++i) {
        std::string sym;
        const Tally t = run(argv[1], static_cast<std::uint16_t>(std::atoi(argv[i])), rule, n, sym);
        all.add(t);
        print(sym.c_str(), t);
    }
    print("TOTAL", all);
    return 0;
}
