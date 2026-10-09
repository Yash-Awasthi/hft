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

using hft::pm::Event;
using hft::pm::Kind;

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

    hft::pm::TokenIndex ids;
    std::vector<std::unique_ptr<hft::pm::TokenMaker>> makers;
    std::vector<bool> seeded;
    auto index = [&](std::string_view id) {
        const std::uint32_t i = ids.get(id);
        if (i == makers.size()) makers.push_back(std::make_unique<hft::pm::TokenMaker>(p)), seeded.push_back(false);
        return i;
    };
    std::vector<std::uint32_t> touched;

    hft::pm::read_records(root, [&](std::int64_t ns, const Event& e) {
        switch (e.kind) {
            case Kind::Book: {
                const std::uint32_t i = index(e.asset);
                hft::pm::TokenMaker& m = *makers[i];
                m.book.clear();
                for (const auto& l : e.bids) m.book.set(true, l.px, l.size);
                for (const auto& l : e.asks) m.book.set(false, l.px, l.size);
                if (e.tick > 0) m.tick = e.tick;
                seeded[i] = true;
                m.on_snapshot(ns);
                break;
            }
            case Kind::PriceChange: {
                touched.clear();
                for (const auto& c : e.changes) {
                    const std::uint32_t i = index(c.asset);
                    if (!seeded[i]) continue;
                    makers[i]->book.set(c.buy, c.px, c.size);
                    if (std::find(touched.begin(), touched.end(), i) == touched.end()) touched.push_back(i);
                }
                for (const std::uint32_t i : touched) makers[i]->on_level(ns);
                break;
            }
            case Kind::Trade: {
                const std::uint32_t i = index(e.asset);
                if (seeded[i]) makers[i]->on_trade(ns, e.buy, e.px, e.size);
                break;
            }
            case Kind::Other: break;
        }
    });

    std::printf("token\tslug\toutcome\tquotes\tbuys\tsells\tshares\tpnl_usd\tend_inventory\tmax_inventory\n");
    double total = 0;
    std::vector<std::pair<std::string, hft::pm::TokenMaker*>> rows;
    for (std::uint32_t i = 0; i < makers.size(); ++i) rows.emplace_back(ids.key(i), makers[i].get());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
        const double x = a.second->shares_traded(), y = b.second->shares_traded();
        return x != y ? x > y : a.first < b.first;
    });
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
