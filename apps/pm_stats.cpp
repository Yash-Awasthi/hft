// Rebuilds the order book of every token in a pm_record directory and prints one row per
// token: message counts, trades, time-weighted spread and depth, and consistency checks
// (crossed books, differences from the exchange's own best bid and offer).
//
//   pm_stats <record-dir> > tokens.tsv

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/json.hpp"
#include "pm/book.hpp"

namespace fs = std::filesystem;
using hft::net::Json;

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

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (p.extension() != ".zst") return s;
    const unsigned long long n = ZSTD_getFrameContentSize(s.data(), s.size());
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("bad frame: " + p.string());
    std::string out(n, '\0');
    const std::size_t got = ZSTD_decompress(out.data(), out.size(), s.data(), s.size());
    if (ZSTD_isError(got)) throw std::runtime_error("decompress failed: " + p.string());
    return out;
}

class Stats {
   public:
    std::unordered_map<std::string, Token> tokens;
    std::uint64_t messages = 0, parse_errors = 0, other = 0;
    double max_gap_s = 0;

    void line(std::string_view l, std::int64_t& last_conn_ns) {
        const std::size_t sp = l.find(' ');
        if (sp == std::string_view::npos) return ++parse_errors, void();
        const std::int64_t ns = std::strtoll(std::string(l.substr(0, sp)).c_str(), nullptr, 10);
        if (last_conn_ns) max_gap_s = std::max(max_gap_s, (ns - last_conn_ns) * 1e-9);
        last_conn_ns = ns;
        ++messages;
        try {
            const Json j = hft::net::parse_json(l.substr(sp + 1));
            if (j.type == Json::Type::Arr)
                for (const Json& e : j.a) event(e, ns);
            else
                event(j, ns);
        } catch (const std::exception&) {
            ++parse_errors;
        }
    }

   private:
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

    static void check(Token& t, const Json& e) {
        const std::string bb = e.str("best_bid"), ba = e.str("best_ask");
        if (bb.empty() || ba.empty() || !t.book.two_sided()) return;
        if (hft::pm::parse_price(bb) != t.book.best_bid() || hft::pm::parse_price(ba) != t.book.best_ask())
        {
            ++t.mismatches;
            if (std::getenv("PM_DEBUG") && t.mismatches < 4)
                std::fprintf(stderr, "mismatch exch %s/%s book %d/%d\n", bb.c_str(), ba.c_str(), t.book.best_bid(),
                             t.book.best_ask());
        }
    }

    void event(const Json& e, std::int64_t ns) {
        const std::string type = e.str("event_type");
        if (type == "book") {
            Token& t = tokens[e.str("asset_id")];
            account(t, ns);
            t.book.clear();
            for (const char* side : {"bids", "asks"})
                if (const Json* lv = e.find(side))
                    for (const Json& l : lv->a)
                        t.book.set(side[0] == 'b', hft::pm::parse_price(l.str("price")),
                                   std::strtod(l.str("size").c_str(), nullptr));
            t.seeded = true;
            ++t.snapshots;
            if (t.book.crossed()) ++t.crossed;
        } else if (type == "price_change") {
            const Json* pc = e.find("price_changes");
            if (!pc) return ++other, void();
            for (const Json& c : pc->a) {
                Token& t = tokens[c.str("asset_id")];
                if (!t.seeded) continue;
                account(t, ns);
                t.book.set(c.str("side") == "BUY", hft::pm::parse_price(c.str("price")),
                           std::strtod(c.str("size").c_str(), nullptr));
                ++t.deltas;
            }
            // The exchange's best bid and offer describe the book after the whole message, so
            // each token is checked once, against its last entry.
            std::vector<const std::string*> done;
            for (std::size_t i = pc->a.size(); i-- > 0;) {
                const Json& c = pc->a[i];
                const Json* id = c.find("asset_id");
                if (!id || std::find_if(done.begin(), done.end(), [&](const std::string* s) { return *s == id->s; }) != done.end())
                    continue;
                done.push_back(&id->s);
                Token& t = tokens[id->s];
                if (!t.seeded) continue;
                if (t.book.crossed()) ++t.crossed;
                check(t, c);
            }
        } else if (type == "last_trade_price") {
            Token& t = tokens[e.str("asset_id")];
            const double sz = std::strtod(e.str("size").c_str(), nullptr);
            ++t.trades;
            t.shares += sz;
            t.notional += sz * std::strtod(e.str("price").c_str(), nullptr);
        } else if (type == "best_bid_ask") {
            Token& t = tokens[e.str("asset_id")];
            (void)t;  // arrives apart from the deltas, so its timing against the book is not defined
        } else {
            ++other;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pm_stats <record-dir>\n");
        return 2;
    }
    const fs::path root = argv[1];
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
    std::vector<fs::path> conns;
    for (const auto& e : fs::directory_iterator(root))
        if (e.is_directory()) conns.push_back(e.path());
    std::sort(conns.begin(), conns.end());
    std::size_t files = 0;
    for (const fs::path& c : conns) {
        std::vector<fs::path> hours;
        for (const auto& e : fs::directory_iterator(c)) hours.push_back(e.path());
        std::sort(hours.begin(), hours.end());
        std::int64_t last = 0;
        for (const fs::path& h : hours) {
            const std::string data = read_file(h);
            ++files;
            std::size_t i = 0;
            while (i < data.size()) {
                std::size_t nl = data.find('\n', i);
                if (nl == std::string::npos) nl = data.size();
                if (nl > i) st.line(std::string_view(data).substr(i, nl - i), last);
                i = nl + 1;
            }
        }
    }

    std::printf("token\ttag\tslug\toutcome\tsnapshots\tdeltas\ttrades\tshares\tnotional\tspread_cents\tdepth5c\t"
                "crossed\tbba_mismatch\tobserved_h\n");
    std::vector<std::pair<std::string, const Token*>> rows;
    for (const auto& [k, t] : st.tokens) rows.emplace_back(k, &t);
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second->notional > b.second->notional; });
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
    std::fprintf(stderr, "files %zu messages %llu parse_errors %llu other %llu tokens %zu max_gap_s %.1f\n", files,
                 (unsigned long long)st.messages, (unsigned long long)st.parse_errors, (unsigned long long)st.other,
                 st.tokens.size(), st.max_gap_s);
    return 0;
}
