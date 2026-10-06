#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace hft {

// ITCH fields are big-endian on the wire.
constexpr std::uint32_t be32_to_host(std::uint32_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(v);
    } else {
        return v;
    }
}

inline std::uint32_t load_be32(const void* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return be32_to_host(v);
}

}  // namespace hft
