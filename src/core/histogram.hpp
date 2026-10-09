#pragma once

// Log-linear latency histogram: values below 2^S are counted exactly, larger ones in
// buckets of relative width 2^-(S-1), so a percentile is within 0.2% of the true value
// for S = 10. Recording is one bit scan and one increment, with no allocation.

#include <bit>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace hft {

class Histogram {
   public:
    static constexpr int S = 10;
    static constexpr std::uint64_t kHalf = 1ull << (S - 1);

    Histogram() : counts_(static_cast<std::size_t>((64 - S + 2) * kHalf), 0) {}

    void record(std::uint64_t v) {
        ++counts_[index(v)];
        ++n_;
        sum_ += v;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
    }
    void reset() {
        std::fill(counts_.begin(), counts_.end(), 0);
        n_ = sum_ = max_ = 0;
        min_ = ~0ull;
    }

    std::uint64_t count() const { return n_; }
    std::uint64_t max() const { return n_ ? max_ : 0; }
    std::uint64_t min() const { return n_ ? min_ : 0; }
    double mean() const { return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0; }

    // Highest value in the bucket holding the p-th percentile (0 < p <= 100), capped at max.
    std::uint64_t percentile(double p) const {
        if (!n_) return 0;
        auto rank = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(n_) + 0.5);
        if (rank < 1) rank = 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                const std::uint64_t hi = i + 1 < counts_.size() ? lowest(i + 1) - 1 : ~0ull;
                return hi < max_ ? hi : max_;
            }
        }
        return max_;
    }

    static std::size_t index(std::uint64_t v) {
        if (v < (1ull << S)) return static_cast<std::size_t>(v);
        const int shift = 63 - std::countl_zero(v) - (S - 1);
        return static_cast<std::size_t>(static_cast<std::uint64_t>(shift) * kHalf + (v >> shift));
    }
    static std::uint64_t lowest(std::size_t i) {
        if (i < (1ull << S)) return i;
        const std::uint64_t shift = i / kHalf - 1, sub = i % kHalf + kHalf;
        return sub << shift;
    }

   private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t n_ = 0, sum_ = 0, max_ = 0, min_ = ~0ull;
};

}  // namespace hft
