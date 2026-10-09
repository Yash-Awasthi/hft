#pragma once

// Order storage for TickBook, index-addressed. The fields touched on every update (price,
// quantity, queue links) sit in 16-byte hot records; reference, arrival sequence and owner
// sit apart in cold records.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/pool.hpp"

namespace hft::book {

namespace detail {

inline void put(std::vector<std::uint8_t>& out, const void* p, std::size_t n) {
    if (n == 0) return;
    const std::size_t at = out.size();
    out.resize(at + n);
    std::memcpy(out.data() + at, p, n);
}

inline bool get(const std::uint8_t*& p, const std::uint8_t* end, void* dst, std::size_t n) {
    if (static_cast<std::size_t>(end - p) < n) return false;
    if (n) std::memcpy(dst, p, n);
    p += n;
    return true;
}

}  // namespace detail

class HotCold {
   public:
    explicit HotCold(std::size_t n) : hot_(n), cold_(n) {}
    void reserve(std::size_t n) {
        hot_.reserve(n);
        cold_.reserve(n);
    }
    std::uint32_t& px(std::uint32_t i) { return hot_[i].px; }
    std::uint32_t& qty(std::uint32_t i) { return hot_[i].qty; }
    std::uint32_t& next(std::uint32_t i) { return hot_[i].next; }
    std::uint32_t& prev(std::uint32_t i) { return hot_[i].prev; }
    std::uint32_t px(std::uint32_t i) const { return hot_[i].px; }
    std::uint32_t qty(std::uint32_t i) const { return hot_[i].qty; }
    std::uint32_t next(std::uint32_t i) const { return hot_[i].next; }
    std::uint32_t prev(std::uint32_t i) const { return hot_[i].prev; }
    std::uint64_t ref(std::uint32_t i) const { return cold_[i].ref; }
    std::uint64_t seq(std::uint32_t i) const { return cold_[i].seq; }
    std::uint32_t owner(std::uint32_t i) const { return cold_[i].owner; }
    void prefetch(std::uint32_t i) const { __builtin_prefetch(&hot_[i]); }
    void set(std::uint32_t i, std::uint32_t px, std::uint32_t qty, std::uint32_t next,
             std::uint32_t prev, std::uint64_t ref, std::uint64_t seq, std::uint32_t owner) {
        hot_[i] = {px, qty, next, prev};
        cold_[i] = {ref, seq, owner, 0};
    }
    void copy_from(const HotCold& o, std::size_t used) {
        hot_.copy_from(o.hot_, used);
        cold_.copy_from(o.cold_, used);
    }
    void save(std::vector<std::uint8_t>& out, std::size_t used) const {
        detail::put(out, hot_.data(), used * sizeof(Hot));
        detail::put(out, cold_.data(), used * sizeof(Cold));
    }
    bool load(const std::uint8_t*& p, const std::uint8_t* end, std::size_t used) {
        reserve(used);
        return detail::get(p, end, hot_.data(), used * sizeof(Hot)) &&
               detail::get(p, end, cold_.data(), used * sizeof(Cold));
    }

   private:
    struct Hot {
        std::uint32_t px, qty, next, prev;
    };
    static_assert(sizeof(Hot) == 16);
    struct Cold {
        std::uint64_t ref, seq;
        std::uint32_t owner, pad;
    };
    Pool<Hot> hot_;
    Pool<Cold> cold_;
};

}  // namespace hft::book
