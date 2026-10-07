#include "strategy/event_transformer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

using namespace hft::strategy;

namespace {

struct Golden {
    std::vector<EventToken> tokens;
    std::vector<float> out;  // per event: forecasts then generator logits
    Golden() {
        std::ifstream f(HFT_SOURCE_DIR "/tests/data/evt_golden.bin", std::ios::binary);
        std::uint32_t n = 0;
        f.read(reinterpret_cast<char*>(&n), 4);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::int32_t v[4];
            float dt;
            f.read(reinterpret_cast<char*>(v), sizeof v);
            f.read(reinterpret_cast<char*>(&dt), 4);
            tokens.push_back({v[0], v[1], v[2], v[3], dt});
        }
        out.assign(n * (6 + EventTransformer::kGen), 0);
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size() * 4));
    }
};

template <bool Avx>
std::vector<float> run(const EventTransformer& m, const std::vector<EventToken>& tokens) {
    auto s = m.state();
    std::vector<float> out;
    float f[6], g[EventTransformer::kGen];
    for (const auto& t : tokens) {
        m.step<Avx>(s, t, f, g);
        out.insert(out.end(), f, f + 6);
        out.insert(out.end(), g, g + EventTransformer::kGen);
    }
    return out;
}

}  // namespace

// Golden test against PyTorch's full-sequence outputs (DESIGN.md section 11): the ring wraps
// several times, so equality also checks that cached keys and values equal a recompute.
TEST(EventTransformer, MatchesPyTorchAndAgreesOnDecisions) {
    const Golden g;
    const EventTransformer m(HFT_SOURCE_DIR "/tests/data/evt_weights.bin");
    ASSERT_EQ(m.forecasts(), 6);
    const std::size_t w = 6 + EventTransformer::kGen;
    for (const auto& got : {run<false>(m, g.tokens), run<true>(m, g.tokens)}) {
        float err = 0;
        std::size_t agree = 0;
        for (std::size_t i = 0; i < got.size(); ++i) err = std::max(err, std::abs(got[i] - g.out[i]));
        for (std::size_t e = 0; e < g.tokens.size(); ++e) {
            const float* a = got.data() + e * w;
            const float* b = g.out.data() + e * w;
            agree += std::max_element(a, a + 3) - a == std::max_element(b, b + 3) - b;
        }
        EXPECT_LT(err, 2e-5f);
        EXPECT_EQ(agree, g.tokens.size());
    }
}

TEST(EventTransformer, BitIdenticalAcrossRuns) {
    const Golden g;
    const EventTransformer m(HFT_SOURCE_DIR "/tests/data/evt_weights.bin");
    const auto a = run<true>(m, g.tokens), b = run<true>(m, g.tokens);
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * 4), 0);
}
