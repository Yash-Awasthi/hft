#include "sources/queue_reactive.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "feed/itch.hpp"
#include "strategy/qr_events.hpp"

using namespace hft;

namespace {

sources::QrParams params(int K = 3, int N = 20) {
    sources::QrParams p;
    p.K = K, p.N = N, p.theta = 0.6;
    const auto cells = static_cast<std::size_t>(K * (N + 1));
    p.L.assign(cells, 0), p.C.assign(cells, 0), p.M.assign(cells, 0), p.init.assign(cells, 0);
    for (int i = 0; i < K; ++i)
        for (int n = 0; n <= N; ++n) {
            const auto c = static_cast<std::size_t>(i * (N + 1) + n);
            p.L[c] = 2.0 / (1 + i);
            p.C[c] = 0.3 * n;
            p.M[c] = i == 0 ? 0.8 : 0.2;
            p.init[c] = n >= 2 && n <= 6 ? 0.2 : 0.0;
        }
    p.start_ns = 34'200'000'000'000ull;
    p.end_ns = p.start_ns + 600'000'000'000ull;
    return p;
}

// Replays the stream, checking that every message applies, the book is never crossed or
// locked and the reference price stays strictly inside the quotes.
struct Replay {
    book::TickBook<> b;
    book::ItchApply<book::TickBook<>> apply{b};
    std::uint64_t msgs = 0, bad_quotes = 0;
    void run(const std::vector<std::uint8_t>& raw) {
        itch::Frame f{};
        for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, f));
             pos += k) {
            ++msgs;
            itch::dispatch(f.data, f.size, apply);
            const book::Bbo q = b.bbo();
            if (q.bid_px && q.ask_px && q.ask_px <= q.bid_px) ++bad_quotes;
        }
    }
};

}  // namespace

TEST(QueueReactive, SameSeedSameBytes) {
    std::vector<std::uint8_t> a, b, c;
    sources::QueueReactive(params(), 11).day(a);
    sources::QueueReactive(params(), 11).day(b);
    sources::QueueReactive(params(), 12).day(c);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(QueueReactive, StreamReplaysCleanlyAndMatchesItsState) {
    sources::QueueReactive sim(params(), 3);
    std::vector<std::uint8_t> raw;
    sim.day(raw);
    Replay r;
    r.run(raw);
    EXPECT_EQ(r.apply.stats.errors, 0u);
    EXPECT_EQ(r.bad_quotes, 0u);
    EXPECT_GT(sim.moves(), 10u);
    const book::Bbo q = r.b.bbo();
    EXPECT_TRUE(!q.bid_px || q.bid_px < sim.p_ref());
    EXPECT_TRUE(!q.ask_px || q.ask_px > sim.p_ref());
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t px =
                s == 0 ? sim.p_ref() - 50 - 100 * static_cast<std::uint32_t>(i)
                       : sim.p_ref() + 50 + 100 * static_cast<std::uint32_t>(i);
            EXPECT_EQ(r.b.level_qty(s == 0 ? book::Side::Buy : book::Side::Sell, px),
                      100u * static_cast<std::uint64_t>(sim.queue(s, i)))
                << s << " " << i;
        }
    // Only the window is resting: everything else was deleted when it left.
    std::uint64_t window = 0;
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 3; ++i) window += static_cast<std::uint64_t>(sim.queue(s, i));
    EXPECT_EQ(r.b.order_count(), window);
}

TEST(QueueReactive, EventCountMatchesAConstantTotalRate) {
    auto p = params(3, 1000);
    for (auto& x : p.C) x = 0;
    for (auto& x : p.M) x = 0;
    for (auto& x : p.L) x = 1.0;
    p.end_ns = p.start_ns + 200'000'000'000ull;
    sources::QueueReactive sim(p, 5);
    std::vector<std::uint8_t> raw;
    sim.day(raw);
    const double expect = 6 * 200.0;  // six levels at one insertion per second
    EXPECT_NEAR(static_cast<double>(sim.events()), expect, 5 * std::sqrt(expect));
}

// The calibration recorder sees exactly the simulator's events: every Gillespie event once,
// redraws only at the instant of a reference move, and one depletion episode per move.
TEST(QueueReactive, RecorderSeesTheSimulatedEventsAndMoves) {
    const auto p = params();
    sources::QueueReactive sim(p, 9);
    std::vector<std::uint8_t> raw;
    sim.day(raw);
    strategy::QrRecorder rec(3, 100, p.start_ns, p.end_ns);
    itch::Frame f{};
    for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, f)); pos += k)
        if (f.data[0] != 'S' && f.data[0] != 'R') rec.on_itch(f.data, f.size);
    const auto& r = rec.record();
    std::uint64_t regular = 0;
    for (std::size_t i = 0; i < r.ts.size(); ++i) {
        regular += !r.after_move[i];
        EXPECT_EQ(r.shares[i], r.kind[i] == 3 ? 0u : 100u);
    }
    EXPECT_EQ(regular, sim.events());
    EXPECT_EQ(r.moves_ts.size(), sim.moves());
    EXPECT_EQ(r.episodes_moved, sim.moves());
    EXPECT_GT(r.episodes_refilled, 0u);
}
