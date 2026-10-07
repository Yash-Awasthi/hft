#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "backtest/intensity.hpp"

using hft::backtest::IntensityEstimator;

namespace {

// Exposure of `secs` at each of the first `bins` distance bins with Poisson fills at A e^{-k d}.
void feed(IntensityEstimator& e, double A, double k, double secs, int bins, std::mt19937_64& rng) {
    for (int b = 0; b < bins; ++b) {
        const double d = b + 0.5;
        e.decay(secs);
        e.expose(d, secs);
        e.fill(d, static_cast<double>(std::poisson_distribution<int>(A * std::exp(-k * d) * secs)(rng)));
    }
}

}  // namespace

TEST(Intensity, NoDataGivesThePrior) {
    IntensityEstimator e(0.3, 0.7, 600, 10);
    e.fit();
    EXPECT_NEAR(e.A(), 0.3, 1e-9);
    EXPECT_NEAR(e.k(), 0.7, 1e-9);
}

TEST(Intensity, RecoversAAndKFromFills) {
    std::mt19937_64 rng(1);
    IntensityEstimator e(2.0, 0.1, 1e15, 1);  // wrong, weak prior
    for (int r = 0; r < 20; ++r) feed(e, 0.8, 0.6, 100, 10, rng);
    e.fit();
    EXPECT_NEAR(e.A() / 0.8, 1, 0.1);
    EXPECT_NEAR(e.k() / 0.6, 1, 0.1);
}

TEST(Intensity, ForgetsAnOldRegime) {
    std::mt19937_64 rng(2);
    IntensityEstimator e(0.8, 0.6, 300, 1);
    for (int r = 0; r < 20; ++r) feed(e, 0.8, 0.6, 100, 10, rng);
    for (int r = 0; r < 20; ++r) feed(e, 0.8, 1.2, 100, 10, rng);
    e.fit();
    EXPECT_NEAR(e.k() / 1.2, 1, 0.15);
}
