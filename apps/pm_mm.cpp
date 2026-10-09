// Runs the logit Avellaneda-Stoikov maker on every token of a pm_record directory and prints
// one row per token. Fills come from recorded trades only (see pm/maker.hpp), makers pay no
// fee, and inventory is valued at the last mid, not at resolution.
//
//   pm_mm <record-dir> [--gamma G] [--k K] [--size S] [--max-inv N] [--horizon-s T] > mm.tsv

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "pm/maker.hpp"
#include "pm/reader.hpp"

using hft::net::Json;
using hft::pm::parse_price;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pm_mm <record-dir> [--gamma G] [--k K] [--size S] [--max-inv N] [--horizon-s T]\n");
        return 2;
    }
    hft::pm::MakerParams p;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const double v = std::atof(argv[i + 1]);
        if (a == "--gamma") p.gamma = v;
        else if (a == "--k") p.k = v;
        else if (a == "--size") p.size = v;
        else if (a == "--max-inv") p.max_inv = v;
        else if (a == "--horizon-s") p.horizon_s = v;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    const std::filesystem::path root = argv[1];
    std::unordered_map<std::string, std::pair<std::string, std::string>> meta;  // token -> slug, outcome
    {
        std::ifstream f(root / "markets.tsv");
        for (std::string l; std::getline(f, l);) {
            std::vector<std::string> c;
            std::stringstream ss(l);
            for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
            if (c.size() >= 7) meta[c[1]] = {c[4], c[6]};
        }
    }

    std::unordered_map<std::string, std::unique_ptr<hft::pm::TokenMaker>> makers;
    auto maker = [&](const std::string& id) -> hft::pm::TokenMaker& {
        auto& m = makers[id];
        if (!m) m = std::make_unique<hft::pm::TokenMaker>(p);
        return *m;
    };
    std::unordered_map<std::string, bool> seeded;

    hft::pm::read_records(root, [&](std::int64_t ns, const Json& e) {
        const std::string type = e.str("event_type");
        if (type == "book") {
            hft::pm::TokenMaker& m = maker(e.str("asset_id"));
            m.book.clear();
            for (const char* side : {"bids", "asks"})
                if (const Json* lv = e.find(side))
                    for (const Json& l : lv->a)
                        m.book.set(side[0] == 'b', parse_price(l.str("price")), std::strtod(l.str("size").c_str(), nullptr));
            if (const std::int32_t t = parse_price(e.str("tick_size")); t > 0) m.tick = t;
            seeded[e.str("asset_id")] = true;
            m.on_snapshot(ns);
        } else if (type == "price_change") {
            const Json* pc = e.find("price_changes");
            if (!pc) return;
            std::vector<std::string> touched;
            for (const Json& c : pc->a) {
                const std::string id = c.str("asset_id");
                if (!seeded[id]) continue;
                maker(id).book.set(c.str("side") == "BUY", parse_price(c.str("price")), std::strtod(c.str("size").c_str(), nullptr));
                if (std::find(touched.begin(), touched.end(), id) == touched.end()) touched.push_back(id);
            }
            for (const std::string& id : touched) maker(id).on_level(ns);
        } else if (type == "last_trade_price") {
            const std::string id = e.str("asset_id");
            if (!seeded[id]) return;
            maker(id).on_trade(ns, e.str("side") == "BUY", parse_price(e.str("price")),
                               std::strtod(e.str("size").c_str(), nullptr));
        }
    });

    std::printf("token\tslug\toutcome\tquotes\tbuys\tsells\tshares\tpnl_usd\tend_inventory\tmax_inventory\n");
    double total = 0;
    std::vector<std::pair<std::string, hft::pm::TokenMaker*>> rows;
    for (auto& [k, m] : makers) rows.emplace_back(k, m.get());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second->shares_traded() > b.second->shares_traded(); });
    for (const auto& [k, m] : rows) {
        const auto it = meta.find(k);
        std::printf("%s\t%s\t%s\t%llu\t%llu\t%llu\t%.0f\t%.2f\t%.0f\t%.0f\n", k.c_str(), it == meta.end() ? "" : it->second.first.c_str(),
                    it == meta.end() ? "" : it->second.second.c_str(), (unsigned long long)m->quotes(),
                    (unsigned long long)m->buys(), (unsigned long long)m->sells(), m->shares_traded(), m->pnl(),
                    m->inventory(), m->max_abs_inventory());
        total += m->pnl();
    }
    std::fprintf(stderr, "tokens %zu total_pnl_usd %.2f\n", rows.size(), total);
    return 0;
}
