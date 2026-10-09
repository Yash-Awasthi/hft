// Moved out of tests/features_test.cpp.
#include "strategy/labeler.hpp"

TEST(Labeler, EventAndClockHorizonsAgainstBruteForce) {
    std::vector<std::uint64_t> ts;
    std::vector<double> mid;
    const rng::Stream g(3, 0, 0);
    std::uint64_t t = 1000;
    for (std::uint32_t i = 0; i < 3000; ++i) {
        t += g.draw(i, 0)[0] % 5'000'000;  // ties allowed when the draw is 0 mod 5e6
        ts.push_back(t);
        mid.push_back(i % 97 == 0 ? std::nan("") : 100 + (g.draw(i, 1)[0] % 21) * 0.5);
    }
    const Horizons h{{1, 10, 100}, {1'000'000, 50'000'000, 1'000'000'000}};
    std::vector<double> y;
    label(ts, mid, h, y);
    const std::size_t w = h.count();
    for (std::size_t i = 0; i < ts.size(); ++i) {
        for (std::size_t c = 0; c < h.events.size(); ++c) {
            const double want =
                i + h.events[c] < ts.size() ? mid[i + h.events[c]] - mid[i] : std::nan("");
            const double got = y[i * w + c];
            EXPECT_TRUE(got == want || (std::isnan(got) && std::isnan(want))) << i << " " << c;
        }
        for (std::size_t c = 0; c < h.clock_ns.size(); ++c) {
            double want = std::nan("");
            if (ts[i] + h.clock_ns[c] <= ts.back()) {
                std::size_t j = i;
                for (std::size_t k = i; k < ts.size() && ts[k] <= ts[i] + h.clock_ns[c]; ++k) j = k;
                want = mid[j] - mid[i];
            }
            const double got = y[i * w + h.events.size() + c];
            EXPECT_TRUE(got == want || (std::isnan(got) && std::isnan(want))) << i << " " << c;
        }
    }
}
