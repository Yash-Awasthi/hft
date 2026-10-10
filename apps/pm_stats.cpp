// Rebuilds the order book of every token in a pm_record directory and prints one row per
// token: message counts, trades, time-weighted spread and depth, and consistency checks
// (crossed books, differences from the exchange's own best bid and offer).
//
//   pm_stats <record-dir> > tokens.tsv

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include "args.hpp"
#include "pm/book.hpp"
#include "pm/reader.hpp"

using hft::pm::Event;
using hft::pm::Kind;

namespace {

struct Token {
    hft::pm::TokenBook book;
    std::uint64_t snapshots = 0, deltas = 0, trades = 0, crossed = 0, mismatches = 0;
    double shares = 0, notional = 0;
    double tw_time = 0, tw_spread = 0, tw_depth = 0;  // seconds, spread-seconds, depth-seconds
    std::int64_t first_ns = 0, last_ns = 0;
    bool seeded = false;  // a snapshot has been seen, so deltas are meaningful
};

struct Meta {
    std::string tag, slug, outcome;
};

class Stats {
   public:
    hft::pm::TokenIndex ids;
    std::vector<std::unique_ptr<Token>> tokens;
    std::uint64_t other = 0;

    void event(const Event& e, std::int64_t ns) {
        switch (e.kind) {
            case Kind::Book: {
                Token& t = token(e.asset);
                account(t, ns);
                t.book.clear();
                for (const auto& l : e.bids) t.book.set(true, l.px, l.size);
                for (const auto& l : e.asks) t.book.set(false, l.px, l.size);
                t.seeded = true;
                ++t.snapshots;
                if (t.book.crossed()) ++t.crossed;
                break;
            }
            case Kind::PriceChange: price_change(e, ns); break;
            case Kind::Trade: {
                Token& t = token(e.asset);
                ++t.trades;
                t.shares += e.size;
                t.notional += e.size * hft::pm::parse_size(e.price);
                break;
            }
            case Kind::Other:
                // best_bid_ask arrives apart from the deltas, so its timing against the book is not defined.
                if (e.type != "best_bid_ask") ++other;
        }
    }

   private:
    Token& token(std::string_view id) {
        const std::uint32_t i = ids.get(id);
        if (i == tokens.size()) tokens.push_back(std::make_unique<Token>());
        return *tokens[i];
    }

    // Credits the time since the token's last change to the state it was in.
    static void account(Token& t, std::int64_t ns) {
        if (t.last_ns && ns > t.last_ns && t.book.two_sided() && !t.book.crossed()) {
            const double dt = (ns - t.last_ns) * 1e-9;
            t.tw_time += dt;
            t.tw_spread += dt * (t.book.best_ask() - t.book.best_bid());
            t.tw_depth += dt * (t.book.depth_bid(500) + t.book.depth_ask(500));
        }
        if (!t.first_ns) t.first_ns = ns;
        t.last_ns = ns;
    }

    static void check(Token& t, const hft::pm::Change& c) {
        if (c.best_bid.empty() || c.best_ask.empty() || !t.book.two_sided()) return;
        if (hft::pm::parse_price(c.best_bid) != t.book.best_bid() || hft::pm::parse_price(c.best_ask) != t.book.best_ask())
            ++t.mismatches;
    }

    void price_change(const Event& e, std::int64_t ns) {
        if (!e.has_changes) {
            ++other;
            return;
        }
        for (const auto& c : e.changes) {
            Token& t = token(c.asset);
            if (!t.seeded) continue;
            account(t, ns);
            t.book.set(c.buy, c.px, c.size);
            ++t.deltas;
        }
        // The exchange's best bid and offer describe the book after the whole message, so
        // each token is checked once, against its last entry.
        done_.clear();
        for (std::size_t i = e.changes.size(); i-- > 0;) {
            const std::uint32_t id = ids.get(e.changes[i].asset);
            if (std::find(done_.begin(), done_.end(), id) != done_.end()) continue;
            done_.push_back(id);
            Token& t = *tokens[id];
            if (!t.seeded) continue;
            if (t.book.crossed()) ++t.crossed;
            check(t, e.changes[i]);
        }
    }

    std::vector<std::uint32_t> done_;
};

}  // namespace

int main(int argc, char** argv) {
    constexpr const char* usage = "usage: pm_stats <record-dir>";
    if (argc != 2) return std::fprintf(stderr, "%s\n", usage), 2;
    if (!hft::app::known_options(argc, argv, {}, usage) || !hft::app::is_dir(argv[1], usage)) return 2;
    const std::filesystem::path root = argv[1];
    std::unordered_map<std::string, Meta> meta;
    {
        std::ifstream f(root / "markets.tsv");
        for (std::string l; std::getline(f, l);) {
            std::vector<std::string> c;
            std::stringstream ss(l);
            for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
            if (c.size() >= 7) meta[c[1]] = {c[3], c[4], c[6]};
        }
    }

    Stats st;
    const auto rs = hft::pm::read_records(root, [&](std::int64_t ns, const Event& e) { st.event(e, ns); });

    std::printf("token\ttag\tslug\toutcome\tsnapshots\tdeltas\ttrades\tshares\tnotional\tspread_cents\tdepth5c\t"
                "crossed\tbba_mismatch\tobserved_h\n");
    std::vector<std::pair<std::string, const Token*>> rows;
    for (std::uint32_t i = 0; i < st.tokens.size(); ++i) rows.emplace_back(st.ids.key(i), st.tokens[i].get());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
        return a.second->notional != b.second->notional ? a.second->notional > b.second->notional : a.first < b.first;
    });
    for (const auto& [k, tp] : rows) {
        const Token& t = *tp;
        const Meta m = meta.count(k) ? meta[k] : Meta{};
        std::printf("%s\t%s\t%s\t%s\t%llu\t%llu\t%llu\t%.0f\t%.0f\t%.3f\t%.0f\t%llu\t%llu\t%.2f\n", k.c_str(), m.tag.c_str(),
                    m.slug.c_str(), m.outcome.c_str(), (unsigned long long)t.snapshots, (unsigned long long)t.deltas,
                    (unsigned long long)t.trades, t.shares, t.notional,
                    t.tw_time > 0 ? t.tw_spread / t.tw_time / 100.0 : 0.0, t.tw_time > 0 ? t.tw_depth / t.tw_time : 0.0,
                    (unsigned long long)t.crossed, (unsigned long long)t.mismatches,
                    (t.last_ns - t.first_ns) / 3.6e12);
    }
    std::fprintf(stderr, "files %zu messages %llu parse_errors %llu other %llu tokens %zu max_gap_s %.1f\n", rs.files,
                 (unsigned long long)rs.messages, (unsigned long long)rs.parse_errors, (unsigned long long)st.other,
                 st.tokens.size(), rs.max_gap_s);
    return 0;
}
