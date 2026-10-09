// Streaming feature run: market events go through the backtest scheduler and reach the
// feature engine as market-data arrivals after a latency, as in the live loop. Writes
// (seq, features) rows of the target symbols as raw little-endian binary, for comparison
// with the batch export of the Python module.
// Usage: feature_stream <store> <latency-ns> <out> <index-locates,> <target-locate>...

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "data/store.hpp"
#include "engine/scheduler.hpp"
#include "feed/itch.hpp"
#include "strategy/multi_features.hpp"

int main(int argc, char** argv) {
    using namespace hft;
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: feature_stream <store> <latency-ns> <out> <index,...> <target>...\n");
        return 2;
    }
    const std::uint64_t latency = std::strtoull(argv[2], nullptr, 10);
    std::vector<std::uint16_t> index, targets;
    for (char* t = std::strtok(argv[4], ","); t; t = std::strtok(nullptr, ","))
        index.push_back(static_cast<std::uint16_t>(std::atoi(t)));
    for (int i = 5; i < argc; ++i)
        targets.push_back(static_cast<std::uint16_t>(std::atoi(argv[i])));
    std::vector<std::uint16_t> all = targets;
    all.insert(all.end(), index.begin(), index.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    auto slot = [&](std::uint16_t loc) {
        return static_cast<std::size_t>(std::lower_bound(all.begin(), all.end(), loc) -
                                        all.begin());
    };
    std::vector<std::size_t> index_slots;
    for (auto l : index) index_slots.push_back(slot(l));
    strategy::MultiFeatures mf(all.size(), index_slots);
    std::vector<bool> is_target(all.size());
    for (auto l : targets) is_target[slot(l)] = true;

    // Messages waiting for their market-data arrival, in arrival order.
    struct Pending {
        std::uint64_t seq;
        std::size_t slot;
        std::vector<std::uint8_t> msg;
    };
    std::deque<Pending> pending;
    engine::EventQueue q(1024);
    std::FILE* out = std::fopen(argv[3], "wb");
    std::vector<double> row(mf.count());
    std::uint32_t next_id = 0, done_id = 0;

    auto drain = [&](std::uint64_t until) {
        engine::Timed t;
        while (q.peek(t) && t.time <= until) {
            q.pop(t);
            const Pending& p = pending[t.payload - done_id];
            if (mf.on_itch(p.slot, p.msg.data(), p.msg.size(), p.seq) && is_target[p.slot]) {
                mf.row(p.slot, mf.last_event().ts, row.data());
                std::fwrite(&p.seq, 8, 1, out);
                std::fwrite(row.data(), 8, row.size(), out);
            }
            pending.pop_front();
            ++done_id;
        }
    };

    data::MergedReader rd(argv[1], all, 64);
    data::Record rec{};
    std::uint16_t loc;
    while (rd.next(rec, loc)) {
        const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
        drain(ts);  // everything due before this exchange event reaches the strategy first
        pending.push_back({rec.seq, slot(loc), {rec.data, rec.data + rec.len}});
        q.push(ts + latency, engine::Kind::MarketData, next_id++);
    }
    drain(~0ull);
    std::fclose(out);
    return 0;
}
