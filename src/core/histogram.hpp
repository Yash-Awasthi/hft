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
    // Only buckets between min and max can be non-zero; the full table is 229 KB.
    void reset() {
        if (n_) std::fill(counts_.begin() + static_cast<std::ptrdiff_t>(index(min_)),
                          counts_.begin() + static_cast<std::ptrdiff_t>(index(max_)) + 1, 0);
        n_ = sum_ = max_ = 0;
        min_ = ~0ull;
    }

    // Adds every value recorded in o.
    void merge(const Histogram& o) {
        if (!o.n_) return;
        for (std::size_t i = index(o.min_), e = index(o.max_); i <= e; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_, sum_ += o.sum_;
        max_ = std::max(max_, o.max_), min_ = std::min(min_, o.min_);
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

    // percentile() for each of ps (ascending) in one scan from the lowest used bucket.
    template <std::size_t N>
    void percentiles(const double (&ps)[N], std::uint64_t (&out)[N]) const {
        std::size_t k = 0, i = n_ ? index(min_) : counts_.size();
        std::uint64_t seen = 0;
        for (; k < N; ++k) {
            auto rank = static_cast<std::uint64_t>(ps[k] / 100.0 * static_cast<double>(n_) + 0.5);
            if (rank < 1) rank = 1;
            for (; i < counts_.size() && seen + counts_[i] < rank; ++i) seen += counts_[i];
            if (i == counts_.size()) break;
            const std::uint64_t hi = i + 1 < counts_.size() ? lowest(i + 1) - 1 : ~0ull;
            out[k] = hi < max_ ? hi : max_;
        }
        for (; k < N; ++k) out[k] = n_ ? max_ : 0;
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
