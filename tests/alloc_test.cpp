// Steady-state replay must not allocate: no operator new and no new pool mappings once the
// book is built with enough capacity. Own binary, since it replaces global operator new.

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "book/itch_apply.hpp"
#include "exec/oms.hpp"
#include "book/tick_book.hpp"
#include "feed/itch.hpp"

namespace {
std::uint64_t g_news = 0;
}

void* operator new(std::size_t n) {
    ++g_news;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    ++g_news;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace hft;

namespace {

struct Enc {
    std::vector<std::uint8_t>& out;
    std::size_t start;
    Enc(std::vector<std::uint8_t>& o, char type) : out(o), start(o.size()) {
        out.push_back(0);
        out.push_back(0);
        out.push_back(static_cast<std::uint8_t>(type));
        n(1, 2).n(0, 2).n(34'200'000'000'000, 6);
    }
    Enc& n(std::uint64_t v, int bytes) {
        for (int s = (bytes - 1) * 8; s >= 0; s -= 8)
            out.push_back(static_cast<std::uint8_t>(v >> s));
        return *this;
    }
    Enc& c(char v) { return n(static_cast<std::uint8_t>(v), 1); }
    ~Enc() {
        const std::size_t len = out.size() - start - 2;
        out[start] = static_cast<std::uint8_t>(len >> 8);
        out[start + 1] = static_cast<std::uint8_t>(len);
    }
};

// A, E, X, D and U messages keeping at most `live` orders around a drifting price.
std::vector<std::uint8_t> stream(std::size_t msgs, std::size_t live) {
    std::vector<std::uint8_t> out;
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> refs;
    std::uint64_t next = 1;
    for (std::size_t i = 0; i < msgs; ++i) {
        const std::uint32_t px = 50'0000 + static_cast<std::uint32_t>(rng() % 200) * 100;
        if (refs.size() < live / 2 || (refs.size() < live && rng() % 2)) {
            Enc(out, 'A')
                .n(next, 8)
                .c(rng() % 2 ? 'B' : 'S')
                .n(100, 4)
                .n(0x41414141, 4)
                .n(0x20202020, 4)
                .n(px, 4);
            refs.push_back(next++);
            continue;
        }
        const std::size_t k = rng() % refs.size();
        switch (rng() % 4) {
            case 0:
                Enc(out, 'E').n(refs[k], 8).n(100, 4).n(i, 8);
                break;
            case 1:
                Enc(out, 'X').n(refs[k], 8).n(100, 4);
                break;
            case 2:
                Enc(out, 'D').n(refs[k], 8);
                break;
            default:
                Enc(out, 'U').n(refs[k], 8).n(next, 8).n(100, 4).n(px, 4);
                refs.push_back(next++);
                break;
        }
        refs[k] = refs.back();
        refs.pop_back();
    }
    return out;
}

}  // namespace

TEST(Alloc, SteadyStateReplayDoesNotAllocate) {
    const std::vector<std::uint8_t> msgs = stream(500'000, 4000);
    book::TickBook b(8192);
    book::ItchApply<book::TickBook> ap{b};

    const std::uint64_t news = g_news, maps = PoolStats::maps;
    itch::Frame f{};
    for (std::size_t pos = 0, k; (k = itch::next_frame(msgs.data() + pos, msgs.size() - pos, f));
         pos += k)
        itch::dispatch(f.data, f.size, ap);
    EXPECT_EQ(g_news - news, 0u);
    EXPECT_EQ(PoolStats::maps - maps, 0u);
    EXPECT_EQ(ap.stats.errors, 0u);
    EXPECT_EQ(ap.stats.book_msgs, 500'000u);
    EXPECT_TRUE(b.check());
}

// Order life cycles through risk, the order manager and the ledger: after a warm-up that grows
// every pool, submit / ack / fill / settle / cancel / free must not allocate.
TEST(Alloc, SteadyStateOrderFlowDoesNotAllocate) {
    using namespace hft::exec;
    constexpr Ns kSec = 1'000'000'000;
    Ledger ledger(1'000'000 * kDollar);
    Exposure exp;
    Risk risk;
    OmsConfig cfg;
    cfg.keep_done = kSec;
    Oms oms(1, risk, exp, ledger, cfg);
    ledger.ensure(2), exp.ensure(2, 1), risk.ensure(2), oms.ensure(2);
    for (std::uint32_t t = 0; t < 2; ++t) ledger.set_group(t, 0), exp.set_group(t, 0), risk.set_group(t, 0);
    pm::MarketRules rules;
    rules.tick = 10;
    const MarketView mv{4000, 4100, true, false, &rules};
    const auto sink = [](const VenueReq&) {};
    const auto on = [](const Order&, OrdEvent, Qty, Px) {};
    Ns now = kSec;
    std::uint64_t fill = 1, accepted = 0;
    auto cycle = [&](int n) {
        for (int i = 0; i < n; ++i) {
            now += kSec / 50;  // 20 ms per order stays under the sustained order rate
            const std::uint64_t cl = oms.submit({0, Side::Buy, Tif::Gtc, false, 0, 4000, 10 * kShare, 0}, mv, now, sink);
            if (!cl) continue;
            ++accepted;
            oms.on_report({VenueRpt::Ack, VenueRpt::Live, 0, 0, 0, 0, cl, 1, 0, now}, now, on);
            oms.on_report({VenueRpt::Fill, VenueRpt::None, 0, 4000, 5 * kShare, 0, cl, 1, fill, now}, now, on);
            oms.on_report({VenueRpt::Settled, VenueRpt::None, 0, 0, 0, 0, cl, 1, fill++, now}, now, on);
            oms.cancel(cl, now, sink);
            oms.on_report({VenueRpt::CancelAck, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on);
            if (i % 100 == 0) oms.on_timer(now, sink);
            ledger.resolve(0, false);  // keep the position, and the risk caps, from building up
        }
    };
    cycle(20'000);  // warm-up: pools reach their working size
    const std::uint64_t news = g_news, maps = PoolStats::maps;
    accepted = 0;
    cycle(50'000);
    const std::uint64_t new_news = g_news - news, new_maps = PoolStats::maps - maps;  // before any EXPECT allocates
    EXPECT_EQ(new_news, 0u);
    EXPECT_EQ(new_maps, 0u);
    EXPECT_EQ(accepted, 50'000u);
    EXPECT_EQ(oms.illegal_count(), 0u);
    EXPECT_FALSE(risk.killed());
}

TEST(Alloc, CounterSeesAllocations) {
    const std::uint64_t news = g_news;
    // A direct call: a new-expression whose result is unused may be elided under LTO.
    void* p = ::operator new(64);
    asm volatile("" : : "r"(p) : "memory");
    ::operator delete(p);
    EXPECT_EQ(g_news - news, 1u);
}
