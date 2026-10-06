// Steady-state replay must not allocate: no operator new and no new pool mappings once the
// book is built with enough capacity. Own binary, since it replaces global operator new.

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "book/itch_apply.hpp"
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
    book::TickBook<book::LinearMap> b(8192);
    book::ItchApply<book::TickBook<book::LinearMap>> ap{b};

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

TEST(Alloc, CounterSeesAllocations) {
    const std::uint64_t news = g_news;
    auto* v = new std::vector<int>(10);
    delete v;
    EXPECT_GE(g_news - news, 2u);
}
