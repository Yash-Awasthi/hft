// Determinism on the committed synthetic fixture: the generator reproduces it byte for byte,
// every book gives the same BBO stream, and the engine's event stream has a fixed hash on
// every compiler. Golden hashes change only with a deliberate change of behaviour.

#include <gtest/gtest.h>
#include <zstd.h>

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/map_book.hpp"
#include "book/tick_book.hpp"
#include "core/philox.hpp"
#include "engine/matching.hpp"
#include "feed/itch.hpp"
#include "sources/random_flow.hpp"

namespace {

using namespace hft;

std::vector<std::uint8_t> fixture() {
    std::ifstream f(HFT_SOURCE_DIR "/tests/data/flow-7-4-20k.itch.zst", std::ios::binary);
    const std::vector<std::uint8_t> comp((std::istreambuf_iterator<char>(f)), {});
    std::vector<std::uint8_t> raw(ZSTD_getFrameContentSize(comp.data(), comp.size()));
    const std::size_t n = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    EXPECT_FALSE(ZSTD_isError(n));
    return raw;
}

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
};

template <class Book>
std::uint64_t bbo_hash(const std::vector<std::uint8_t>& raw) {
    std::map<std::uint16_t, Book> books;
    Fnv h;
    itch::Frame f{};
    for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, f));
         pos += k) {
        const std::uint16_t loc = load_be16(f.data + 1);
        Book& b = books[loc];
        book::ItchApply<Book> ap{b};
        itch::dispatch(f.data, f.size, ap);
        EXPECT_EQ(ap.stats.errors, 0u);
        const book::Bbo bbo = b.bbo();
        h.add(&loc, 2);
        h.add(&bbo, sizeof bbo);
    }
    return h.h;
}

// Engine driven by Philox commands; the hash covers every event field.
std::uint64_t engine_hash(engine::MatchingEngine<>& m, std::uint32_t from, std::uint32_t to,
                          std::vector<std::uint64_t>& refs) {
    using namespace engine;
    const rng::Stream r(11, 0, 0);
    Fnv h;
    auto sink = [&](const Event& e) {
        h.add(&e.type, 1);
        h.add(&e.reason, 1);
        h.add(&e.owner, 4);
        h.add(&e.ref, 8);
        h.add(&e.price, 4);
        h.add(&e.qty, 4);
        h.add(&e.fee, 8);
        h.add(&e.match, 8);
    };
    for (std::uint32_t i = from; i < to; ++i) {
        const auto d = r.draw(i, 0);
        const std::uint32_t owner = 1 + d[0] % 5;
        const Side side = d[1] & 1 ? Side::Sell : Side::Buy;
        const std::uint32_t px = 50'0000 + (d[2] % 21) * 100 - 1000;
        if (d[3] % 5 == 0 && !refs.empty()) {
            m.cancel(refs[d[3] % refs.size()], owner, sink);
        } else {
            const Tif tif = d[3] % 7 == 1 ? Tif::Ioc : Tif::Day;
            refs.push_back(m.submit({owner, side, px, 1 + d[0] % 500, tif, d[3] % 11 == 2}, sink));
        }
    }
    return h.h;
}

}  // namespace

TEST(Fixture, GeneratorReproducesCommittedBytes) {
    std::vector<std::uint8_t> regen;
    sources::RandomFlow(7, 4).day(20000, regen);
    EXPECT_EQ(regen, fixture());
}

TEST(Fixture, AllBooksGiveTheGoldenBboStream) {
    const auto raw = fixture();
    const std::uint64_t want = bbo_hash<book::MapBook>(raw);
    EXPECT_EQ(bbo_hash<book::TickBook<>>(raw), want);
    EXPECT_EQ((bbo_hash<book::TickBook<book::RobinHoodMap, book::Soa>>(raw)), want);
    EXPECT_EQ(want, 0xab517e7049413acaull) << std::hex << want;
}

TEST(Fixture, EngineEventStreamIsGoldenAndForkReplaysIdentically) {
    engine::Config c;
    c.maker_rebate = 2000;
    c.taker_fee = 3000;
    engine::MatchingEngine<> a(c), b(c);
    std::vector<std::uint64_t> ra, rb;
    const std::uint64_t first = engine_hash(a, 0, 5000, ra);
    b.copy_from(a);
    rb = ra;
    const std::uint64_t second_a = engine_hash(a, 5000, 10000, ra);
    const std::uint64_t second_b = engine_hash(b, 5000, 10000, rb);
    EXPECT_EQ(second_a, second_b);
    EXPECT_EQ(first, 0x90c5635ce2c4ac0eull) << std::hex << first;
    EXPECT_EQ(second_a, 0x42851fe80b6ec970ull) << std::hex << second_a;
}
