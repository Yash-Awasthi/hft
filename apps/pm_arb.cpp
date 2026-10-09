// Replays pm_live recordings through the books and the arbitrage scanner only (no makers)
// and lists every window in which an event's Yes asks summed below 1 or its bids above 1,
// or a market's Yes and No did.
//
//   pm_arb [--min-ms M] <run-dir | dir of run dirs> ... > windows.tsv
//
// Windows shorter than M milliseconds (default 1) are dropped: they come from the two halves
// of one price change arriving in separate messages, not from a price anyone could trade.
//
// stdout: one row per window. stderr: per-group totals over all runs. Edges are gross, in
// units of 0.0001 per set of shares; fees and the cost of hitting several books in turn
// are not counted.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pm/reader.hpp"
#include "pm/session.hpp"

namespace fs = std::filesystem;
using namespace hft::pm;

namespace {

struct Totals {
    std::uint64_t windows = 0, buy = 0;
    double open_s = 0, size = 0;
    std::int32_t max_edge = 0;
    std::vector<double> durations_ms;
};

void runs_under(const fs::path& p, std::vector<fs::path>& out) {
    if (fs::exists(p / "session.tsv")) {
        out.push_back(p);
        return;
    }
    std::vector<fs::path> kids;
    for (const auto& e : fs::directory_iterator(p))
        if (e.is_directory()) kids.push_back(e.path());
    std::sort(kids.begin(), kids.end());
    for (const auto& k : kids) runs_under(k, out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pm_arb [--min-ms M] <run-dir | dir of run dirs> ...\n");
        return 2;
    }
    std::vector<fs::path> runs;
    double min_ms = 1;
    std::uint64_t dropped = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--min-ms" && i + 1 < argc) min_ms = std::atof(argv[++i]);
        else runs_under(argv[i], runs);
    }
    std::map<std::string, Totals> totals;
    std::uint64_t messages = 0;
    std::printf("run\tgroup\tside\topen_ns\tduration_ms\topen_edge\tmax_edge\tsize_at_max\n");
    for (const fs::path& run : runs) {
        try {
            const Session s = Session::load(run / "session.tsv");
            EngineParams p;
            p.quote = false;
            Engine e(p);
            s.apply(e);
            std::int64_t last = 0;
            const auto st = read_records(run, [&](std::int64_t ns, const Event& ev) {
                e.on_event(ns, ev);
                last = ns;
            });
            messages += st.messages;
            e.finish(last);
            const auto& names = e.arb().stats();
            for (const ArbWindow& w : e.arb_closed()) {
                const std::string& g = names[w.group].name;
                const double ms = static_cast<double>(w.close_ns - w.open_ns) * 1e-6;
                if (ms < min_ms) {
                    ++dropped;
                    continue;
                }
                std::printf("%s\t%s\t%s\t%lld\t%.3f\t%d\t%d\t%.2f\n", run.filename().c_str(), g.c_str(),
                            w.buy ? "asks<1" : "bids>1", static_cast<long long>(w.open_ns), ms, w.open_edge,
                            w.max_edge, w.size_at_max);
                Totals& t = totals[g];
                ++t.windows, t.buy += w.buy;
                t.open_s += ms * 1e-3;
                t.size += w.size_at_max;
                t.max_edge = std::max(t.max_edge, w.max_edge);
                t.durations_ms.push_back(ms);
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "%s: %s\n", run.c_str(), ex.what());
        }
    }
    std::fprintf(stderr, "runs %zu messages %llu windows_under_%gms %llu\n", runs.size(),
                 static_cast<unsigned long long>(messages), min_ms, static_cast<unsigned long long>(dropped));
    std::fprintf(stderr, "group\twindows\tasks<1\topen_s\tmedian_ms\tmax_edge\tmean_size\n");
    for (auto& [g, t] : totals) {
        std::sort(t.durations_ms.begin(), t.durations_ms.end());
        std::fprintf(stderr, "%s\t%llu\t%llu\t%.3f\t%.1f\t%d\t%.1f\n", g.c_str(), static_cast<unsigned long long>(t.windows),
                     static_cast<unsigned long long>(t.buy), t.open_s, t.durations_ms[t.durations_ms.size() / 2],
                     t.max_edge, t.size / static_cast<double>(t.windows));
    }
    return 0;
}
