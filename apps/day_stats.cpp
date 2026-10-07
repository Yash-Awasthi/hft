// Per-symbol statistics of one day for universe selection, over regular hours (09:30 to
// 16:00): time-weighted spread in ticks, share of time at one tick, mean mid, traded shares
// and notional, message count, ETP flag and market category from the directory message.
// Usage: day_stats <store-dir> > stats.tsv

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft;

constexpr std::uint64_t kOpen = 34'200'000'000'000ull;   // 09:30
constexpr std::uint64_t kClose = 57'600'000'000'000ull;  // 16:00

struct Stats {
    std::string symbol;
    char etp = '?', category = '?';
    std::uint64_t msgs = 0, shares = 0;
    double notional = 0, spread_ticks_t = 0, one_tick_t = 0, mid_t = 0, two_sided_t = 0;
};

struct Meta {
    Stats& s;
    void operator()(const itch::StockDirectory& m) {
        s.symbol.assign(m.stock, 8);
        s.symbol.erase(s.symbol.find_last_not_of(' ') + 1);
        s.etp = m.etp_flag;
        s.category = m.market_category;
    }
    template <class T>
    void operator()(const T&) {}
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: day_stats <store-dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::vector<std::uint16_t> locs;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".idx")
            locs.push_back(static_cast<std::uint16_t>(std::stoul(e.path().stem().string())));
    std::sort(locs.begin(), locs.end());
    std::printf(
        "locate\tsymbol\tmsgs\tshares\tnotional\tspread_ticks\tone_tick_share\tmid\ttwo_sided_"
        "share\tetp\tcategory\n");
    for (std::uint16_t loc : locs) {
        if (loc == 0) continue;
        book::TickBook<> b(4096);
        book::ItchApply<book::TickBook<>> ap{b};
        Stats s;
        Meta meta{s};
        data::SymbolReader rd(dir, loc);
        data::Record rec{};
        std::uint64_t last_ts = kOpen;
        book::Bbo bbo{};
        auto accrue = [&](std::uint64_t to) {
            to = std::min(to, kClose);
            if (to <= last_ts) return;
            const double dt = static_cast<double>(to - last_ts) * 1e-9;
            last_ts = to;
            if (!bbo.bid_px || !bbo.ask_px || bbo.ask_px <= bbo.bid_px) return;
            const double ticks = (bbo.ask_px - bbo.bid_px) / 100.0;
            s.two_sided_t += dt;
            s.spread_ticks_t += ticks * dt;
            s.one_tick_t += (bbo.ask_px - bbo.bid_px == 100) * dt;
            s.mid_t += (bbo.ask_px + bbo.bid_px) / 2e4 * dt;
        };
        while (rd.next(rec)) {
            ++s.msgs;
            const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
            if (ts > kOpen) accrue(ts);
            itch::dispatch(rec.data, rec.len, meta);
            const std::uint64_t before = ap.stats.executed + ap.stats.hidden;
            if (const char t = static_cast<char>(rec.data[0]); t == 'E' || t == 'C') {
                // Notional of executions at the resting order's price.
                const std::uint64_t ref = load_be64(rec.data + 11);
                if (const auto o = b.order(ref); o && ts >= kOpen && ts < kClose)
                    s.notional += o->price / 1e4 * load_be32(rec.data + 19);
            }
            itch::dispatch(rec.data, rec.len, ap);
            if (ts >= kOpen && ts < kClose)
                s.shares += ap.stats.executed + ap.stats.hidden - before;
            bbo = b.bbo();
        }
        accrue(kClose);
        const double t = s.two_sided_t > 0 ? s.two_sided_t : 1;
        std::printf("%u\t%s\t%llu\t%llu\t%.0f\t%.4f\t%.4f\t%.4f\t%.4f\t%c\t%c\n", loc,
                    s.symbol.empty() ? "-" : s.symbol.c_str(), (unsigned long long)s.msgs,
                    (unsigned long long)s.shares, s.notional, s.spread_ticks_t / t,
                    s.one_tick_t / t, s.mid_t / t, s.two_sided_t / ((kClose - kOpen) * 1e-9), s.etp,
                    s.category);
    }
    return 0;
}
