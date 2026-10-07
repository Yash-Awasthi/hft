#include "core/philox.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace hft::rng;

// Known answers from Random123 tests/kat_vectors (philox4x32, 10 rounds).
TEST(Philox, KnownAnswers) {
    struct Kat {
        Ctr ctr;
        Key key;
        Ctr out;
    };
    const Kat kats[] = {
        {{0, 0, 0, 0}, {0, 0}, {0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8}},
        {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
         {0xffffffff, 0xffffffff},
         {0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd}},
        {{0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344},
         {0xa4093822, 0x299f31d0},
         {0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1}},
    };
    for (const Kat& k : kats) EXPECT_EQ(philox(k.ctr, k.key), k.out);
}

TEST(Philox, Avx2MatchesScalar) {
    if (!has_avx2()) GTEST_SKIP() << "no AVX2";
    std::mt19937 g(5);
    for (int rep = 0; rep < 100; ++rep) {
        Lanes ctr, out;
        Key key{static_cast<std::uint32_t>(g()), static_cast<std::uint32_t>(g())};
        for (int w = 0; w < 4; ++w)
            for (int l = 0; l < 8; ++l) ctr.v[w][l] = static_cast<std::uint32_t>(g());
        philox_x8(ctr, key, out);
        for (int l = 0; l < 8; ++l) {
            const Ctr want = philox({ctr.v[0][l], ctr.v[1][l], ctr.v[2][l], ctr.v[3][l]}, key);
            for (int w = 0; w < 4; ++w) EXPECT_EQ(out.v[w][l], want[w]);
        }
    }
}

// Draws are addressed, not sequential: the same address gives the same numbers whatever
// was drawn before, and each component of the address changes them.
TEST(Philox, StreamIsAddressed) {
    const Stream a(42, 7, 3), b(42, 7, 3);
    a.draw(99, 1);
    EXPECT_EQ(a.draw(5, 2), b.draw(5, 2));
    EXPECT_NE(a.draw(5, 2), Stream(43, 7, 3).draw(5, 2));
    EXPECT_NE(a.draw(5, 2), Stream(42, 8, 3).draw(5, 2));
    EXPECT_NE(a.draw(5, 2), Stream(42, 7, 4).draw(5, 2));
    EXPECT_NE(a.draw(5, 2), a.draw(6, 2));
    EXPECT_NE(a.draw(5, 2), a.draw(5, 3));
    for (std::uint32_t e = 0; e < 1000; ++e) {
        const double u = a.uniform(e, 0);
        EXPECT_GT(u, 0.0);
        EXPECT_LT(u, 1.0);
    }
}
