#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace hft {

// ITCH fields are big-endian on the wire.
constexpr std::uint16_t be16_to_host(std::uint16_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(v);
    } else {
        return v;
    }
}

constexpr std::uint32_t be32_to_host(std::uint32_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(v);
    } else {
        return v;
    }
}

constexpr std::uint64_t be64_to_host(std::uint64_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(v);
    } else {
        return v;
    }
}

template <class T>
inline T load_raw(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline std::uint16_t load_be16(const void* p) { return be16_to_host(load_raw<std::uint16_t>(p)); }
inline std::uint32_t load_be32(const void* p) { return be32_to_host(load_raw<std::uint32_t>(p)); }
inline std::uint64_t load_be64(const void* p) { return be64_to_host(load_raw<std::uint64_t>(p)); }

inline std::uint64_t load_be48(const std::uint8_t* p) {
    return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

}  // namespace hft
