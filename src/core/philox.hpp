#pragma once

// Philox4x32-10 counter-based RNG (Salmon et al., SC'11). Draws are addressed by (seed, pair,
// rollout, event, purpose), so the two arms of an intervention pair draw identical numbers for
// the same event even when one arm makes more draws than the other.

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
