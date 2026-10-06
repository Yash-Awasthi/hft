#pragma once

// Index-addressed array of trivially copyable T in its own anonymous mapping. Indices stay
// valid across growth, so a state built from pools can be copied with memcpy. Mappings of
// 2 MB or more are aligned to 2 MB, advised for transparent huge pages and pre-faulted;
// MAP_POPULATE would fault 4 KB pages before the advice applies.

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

namespace hft {

struct PoolStats {
    static inline bool huge_pages = true;
    static inline std::uint64_t maps = 0;  // mappings created, growth included
    static inline std::uint64_t bytes = 0;
};

template <class T>
class Pool {
    static_assert(std::is_trivially_copyable_v<T>);

   public:
    explicit Pool(std::size_t capacity = 0) { reserve(capacity); }
    ~Pool() { unmap(); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&& o) noexcept { *this = std::move(o); }
    Pool& operator=(Pool&& o) noexcept {
        if (this != &o) {
            unmap();
            base_ = std::exchange(o.base_, nullptr);
            map_len_ = std::exchange(o.map_len_, 0);
            data_ = std::exchange(o.data_, nullptr);
            cap_ = std::exchange(o.cap_, 0);
        }
        return *this;
    }

    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    std::size_t capacity() const { return cap_; }

    // Grows to at least n elements, keeping contents. New elements are zero.
    void reserve(std::size_t n) {
        if (n <= cap_) return;
        std::size_t want = cap_ ? cap_ : 64;
        while (want < n) want *= 2;
        const std::size_t len = want * sizeof(T);
        const bool huge = PoolStats::huge_pages && len >= kHuge;
        const std::size_t map_len = huge ? len + kHuge : len;
        void* p =
            ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) throw std::bad_alloc();
        auto* d = static_cast<std::uint8_t*>(p);
        if (huge) {
            d += (kHuge - reinterpret_cast<std::uintptr_t>(d) % kHuge) % kHuge;
            ::madvise(d, len, MADV_HUGEPAGE);
        }
        ::madvise(d, len, MADV_POPULATE_WRITE);
        if (cap_) std::memcpy(d, data_, cap_ * sizeof(T));
        unmap();
        base_ = p;
        map_len_ = map_len;
        data_ = reinterpret_cast<T*>(d);
        cap_ = want;
        ++PoolStats::maps;
        PoolStats::bytes += map_len;
    }

   private:
    static constexpr std::size_t kHuge = 2u << 20;

    void unmap() {
        if (base_) ::munmap(base_, map_len_);
        base_ = nullptr;
    }

    void* base_ = nullptr;
    std::size_t map_len_ = 0;
    T* data_ = nullptr;
    std::size_t cap_ = 0;
};

}  // namespace hft
