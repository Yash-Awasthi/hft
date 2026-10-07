#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace hft::backtest {

// Online fill intensity lambda(d) = A exp(-k d) at quote distance d (ticks): exposure seconds
// and fills per one-tick distance bin, forgotten with time constant tau, fitted by Poisson
// maximum likelihood. The prior A0, k0 enters as `prior_s` seconds of pseudo-exposure per bin
// with its expected fills; it is not forgotten, so the fit falls back to it without data.
class IntensityEstimator {
   public:
    static constexpr int kBins = 32;

    IntensityEstimator() = default;
    IntensityEstimator(double A0, double k0, double tau_s, double prior_s)
        : tau_(tau_s), a_(std::log(A0)), k_(k0) {
        for (int b = 0; b < kBins; ++b) {
            prior_e_[b] = prior_s;
            prior_n_[b] = prior_s * A0 * std::exp(-k0 * center(b));
        }
    }

    void decay(double dt) {
        if (dt <= 0) return;
        const double w = std::exp(-dt / tau_);
        for (int b = 0; b < kBins; ++b) e_[b] *= w, n_[b] *= w;
    }
    void expose(double d, double dt) { e_[bin(d)] += dt; }
    void fill(double d, double n) { n_[bin(d)] += n; }

    // Newton steps on (a = ln A, k); the log-likelihood is concave in both.
    void fit(int iters = 30) {
        for (int it = 0; it < iters; ++it) {
            double ga = 0, gk = 0, haa = 0, hak = 0, hkk = 0;
            for (int b = 0; b < kBins; ++b) {
                const double x = center(b), n = n_[b] + prior_n_[b];
                const double mu = (e_[b] + prior_e_[b]) * std::exp(a_ - k_ * x);
                ga += n - mu;
                gk += x * (mu - n);
                haa += mu, hak += x * mu, hkk += x * x * mu;
            }
            // Solve [haa -hak; -hak hkk] [da dk] = [ga gk] (negative Hessian).
            const double det = haa * hkk - hak * hak;
            if (!(det > 0)) break;
            const double da = (hkk * ga + hak * gk) / det, dk = (hak * ga + haa * gk) / det;
            a_ += std::clamp(da, -1.0, 1.0);
            k_ = std::clamp(k_ + std::clamp(dk, -0.5, 0.5), 1e-3, 10.0);
            if (std::abs(da) < 1e-10 && std::abs(dk) < 1e-10) break;
        }
    }

    double A() const { return std::exp(a_); }
    double k() const { return k_; }

   private:
    static int bin(double d) { return std::clamp(static_cast<int>(std::floor(d)), 0, kBins - 1); }
    static double center(int b) { return b + 0.5; }

    std::array<double, kBins> e_{}, n_{}, prior_e_{}, prior_n_{};
    double tau_ = 600, a_ = 0, k_ = 1;
};

}  // namespace hft::backtest
