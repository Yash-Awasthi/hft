#pragma once

// Philox4x32-10 counter-based RNG (Salmon et al., SC'11), scalar and eight blocks at a time
// with AVX2. Draws are addressed by (seed, pair, rollout, event, purpose), so the two arms of
// an intervention pair draw identical numbers for the same event even when one arm makes
// more draws than the other.

#include <immintrin.h>

#include <array>
#include <cstdint>

namespace hft::rng {

using Ctr = std::array<std::uint32_t, 4>;
using Key = std::array<std::uint32_t, 2>;

inline constexpr std::uint32_t kM0 = 0xD2511F53, kM1 = 0xCD9E8D57;
inline constexpr std::uint32_t kW0 = 0x9E3779B9, kW1 = 0xBB67AE85;

constexpr Ctr philox(Ctr x, Key k) {
    for (int r = 0; r < 10; ++r) {
        if (r) k = {k[0] + kW0, k[1] + kW1};
        const std::uint64_t p0 = std::uint64_t{kM0} * x[0];
        const std::uint64_t p1 = std::uint64_t{kM1} * x[2];
        x = {static_cast<std::uint32_t>(p1 >> 32) ^ x[1] ^ k[0], static_cast<std::uint32_t>(p1),
             static_cast<std::uint32_t>(p0 >> 32) ^ x[3] ^ k[1], static_cast<std::uint32_t>(p0)};
    }
    return x;
}

// Eight counters as structure of arrays: v[word][lane].
struct alignas(32) Lanes {
    std::uint32_t v[4][8];
};

inline bool has_avx2() { return __builtin_cpu_supports("avx2"); }

namespace detail {

__attribute__((target("avx2"))) inline void mulhilo(__m256i m, __m256i x, __m256i& lo,
                                                    __m256i& hi) {
    const __m256i even = _mm256_mul_epu32(m, x);
    const __m256i odd = _mm256_mul_epu32(m, _mm256_srli_epi64(x, 32));
    lo = _mm256_blend_epi32(even, _mm256_slli_epi64(odd, 32), 0xAA);
    hi = _mm256_blend_epi32(_mm256_srli_epi64(even, 32), odd, 0xAA);
}

}  // namespace detail

// Same result as philox() on each lane. Callers check has_avx2() first.
__attribute__((target("avx2"))) inline void philox_x8(const Lanes& in, Key key, Lanes& out) {
    __m256i x0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in.v[0]));
    __m256i x1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in.v[1]));
    __m256i x2 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in.v[2]));
    __m256i x3 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in.v[3]));
    const __m256i m0 = _mm256_set1_epi32(static_cast<int>(kM0));
    const __m256i m1 = _mm256_set1_epi32(static_cast<int>(kM1));
    for (int r = 0; r < 10; ++r) {
        if (r) key = {key[0] + kW0, key[1] + kW1};
        __m256i lo0, hi0, lo1, hi1;
        detail::mulhilo(m0, x0, lo0, hi0);
        detail::mulhilo(m1, x2, lo1, hi1);
        const __m256i k0 = _mm256_set1_epi32(static_cast<int>(key[0]));
        const __m256i k1 = _mm256_set1_epi32(static_cast<int>(key[1]));
        x0 = _mm256_xor_si256(_mm256_xor_si256(hi1, x1), k0);
        x1 = lo1;
        x2 = _mm256_xor_si256(_mm256_xor_si256(hi0, x3), k1);
        x3 = lo0;
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(out.v[0]), x0);
    _mm256_store_si256(reinterpret_cast<__m256i*>(out.v[1]), x1);
    _mm256_store_si256(reinterpret_cast<__m256i*>(out.v[2]), x2);
    _mm256_store_si256(reinterpret_cast<__m256i*>(out.v[3]), x3);
}

// Key: the 64-bit experiment seed. Counter: (event, purpose, pair, rollout).
class Stream {
   public:
    Stream(std::uint64_t seed, std::uint32_t pair, std::uint32_t rollout)
        : key_{static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32)},
          pair_(pair),
          rollout_(rollout) {}

    Ctr draw(std::uint32_t event, std::uint32_t purpose) const {
        return philox({event, purpose, pair_, rollout_}, key_);
    }

    // Uniform on the open interval (0, 1) from the k-th word of the draw.
    double uniform(std::uint32_t event, std::uint32_t purpose, int k = 0) const {
        return (draw(event, purpose)[static_cast<std::size_t>(k)] + 0.5) * 0x1p-32;
    }

   private:
    Key key_;
    std::uint32_t pair_, rollout_;
};

}  // namespace hft::rng
