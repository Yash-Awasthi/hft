#pragma once

// Accumulators for the regime forecast inputs (DESIGN.md section 5).
// Eta (Robert and Rosenbaum; Dayri and Rosenbaum 2015): continuations over twice the
// alternations among successive non-zero mid changes. Realized variance from the mid sampled on
// a fixed clock.

#include <cmath>
#include <cstdint>

namespace hft::strategy {

class EtaCounter {
   public:
    void on_mid(double mid) {
        if (!std::isfinite(mid)) return;
        if (std::isfinite(last_) && mid != last_) {
            const int dir = mid > last_ ? 1 : -1;
            if (prev_dir_) (dir == prev_dir_ ? cont_ : alt_)++;
            prev_dir_ = dir;
        }
        last_ = mid;
    }
    std::uint64_t continuations() const { return cont_; }
    std::uint64_t alternations() const { return alt_; }
    double eta() const { return alt_ ? static_cast<double>(cont_) / (2.0 * alt_) : NAN; }

   private:
    double last_ = NAN;
    int prev_dir_ = 0;
    std::uint64_t cont_ = 0, alt_ = 0;
};

// Mid sampled at multiples of step_ns from start_ns: the value holding at each grid point.
class ClockVariance {
   public:
    ClockVariance(std::uint64_t start_ns, std::uint64_t step_ns) : next_(start_ns), step_(step_ns) {}
    // Call before the mid changes at ts, with the mid that held until ts.
    void advance(std::uint64_t ts, double mid_before) {
        for (; next_ <= ts; next_ += step_) {
            if (std::isfinite(mid_before) && std::isfinite(sampled_)) {
                const double d = mid_before - sampled_;
                sum_ += d * d;
                ++n_;
            }
            sampled_ = mid_before;
        }
    }
    double variance() const { return sum_; }  // sum of squared changes
    std::uint64_t samples() const { return n_; }

   private:
    std::uint64_t next_, step_;
    double sampled_ = NAN, sum_ = 0;
    std::uint64_t n_ = 0;
};

}  // namespace hft::strategy
