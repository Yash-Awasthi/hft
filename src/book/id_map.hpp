#pragma once

// Order-ID maps from a 64-bit ITCH reference to a 32-bit order index. Both are open
// addressing over a power-of-two table, kept at most half full, with backward-shift
// deletion so no tombstones build up over a day.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "core/pool.hpp"

namespace hft::book {

inline constexpr std::uint32_t kNoOrder = 0xffffffffu;

namespace detail {

struct Slot {
    std::uint64_t key;
    std::uint32_t val;
    std::uint32_t pad;
};
inline constexpr std::uint64_t kEmpty = ~0ull;

// Shared table storage; Derived supplies insert, find and erase.
class Table {
   public:
    explicit Table(std::size_t expected) {
        rebuild(std::bit_ceil(expected < 32 ? 64 : expected * 2));
    }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return mask_ + 1; }
    void clear() {
        for (std::size_t i = 0; i <= mask_; ++i) slots_[i].key = kEmpty;
        size_ = 0;
    }
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i <= mask_; ++i)
            if (slots_[i].key != kEmpty) f(slots_[i].key, slots_[i].val);
    }

   protected:
    std::size_t home(std::uint64_t key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    std::size_t dist(std::size_t i, std::uint64_t key) const { return (i - home(key)) & mask_; }

    void rebuild(std::size_t cap) {
        slots_ = Pool<Slot>(cap);
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
        for (std::size_t i = 0; i < cap; ++i) slots_[i].key = kEmpty;
    }

    Pool<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace detail

class LinearMap : public detail::Table {
   public:
    explicit LinearMap(std::size_t expected = 1024) : Table(expected) {}

    std::uint32_t find(std::uint64_t key) const {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const detail::Slot& s = slots_[i];
            if (s.key == key) return s.val;
            if (s.key == detail::kEmpty) return kNoOrder;
        }
    }

    // `key` must be absent.
    void insert(std::uint64_t key, std::uint32_t val) {
        if ((size_ + 1) * 2 > capacity()) grow();
        std::size_t i = home(key);
        while (slots_[i].key != detail::kEmpty) i = (i + 1) & mask_;
        slots_[i] = {key, val, 0};
        ++size_;
    }

    bool erase(std::uint64_t key) {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].key == key) break;
            if (slots_[i].key == detail::kEmpty) return false;
        }
        // Pull back any later entry whose probe path crosses the hole.
        for (std::size_t j = (i + 1) & mask_; slots_[j].key != detail::kEmpty;
             j = (j + 1) & mask_) {
            if (dist(j, slots_[j].key) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = detail::kEmpty;
        --size_;
        return true;
    }

   private:
    void grow() {
        Pool<detail::Slot> old = std::move(slots_);
        const std::size_t n = capacity();
        rebuild(n * 2);
        size_ = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (old[i].key != detail::kEmpty) insert(old[i].key, old[i].val);
    }
};

class RobinHoodMap : public detail::Table {
   public:
    explicit RobinHoodMap(std::size_t expected = 1024) : Table(expected) {}

    std::uint32_t find(std::uint64_t key) const {
        for (std::size_t i = home(key), d = 0;; i = (i + 1) & mask_, ++d) {
            const detail::Slot& s = slots_[i];
            if (s.key == key) return s.val;
            if (s.key == detail::kEmpty || dist(i, s.key) < d) return kNoOrder;
        }
    }

    void insert(std::uint64_t key, std::uint32_t val) {
        if ((size_ + 1) * 2 > capacity()) grow();
        detail::Slot cur{key, val, 0};
        for (std::size_t i = home(key), d = 0;; i = (i + 1) & mask_, ++d) {
            detail::Slot& s = slots_[i];
            if (s.key == detail::kEmpty) {
                s = cur;
                break;
            }
            const std::size_t sd = dist(i, s.key);
            if (sd < d) {
                std::swap(s, cur);
                d = sd;
            }
        }
        ++size_;
    }

    bool erase(std::uint64_t key) {
        std::size_t i = home(key);
        for (std::size_t d = 0;; i = (i + 1) & mask_, ++d) {
            const detail::Slot& s = slots_[i];
            if (s.key == key) break;
            if (s.key == detail::kEmpty || dist(i, s.key) < d) return false;
        }
        for (std::size_t j = (i + 1) & mask_;
             slots_[j].key != detail::kEmpty && dist(j, slots_[j].key) != 0; j = (j + 1) & mask_) {
            slots_[i] = slots_[j];
            i = j;
        }
        slots_[i].key = detail::kEmpty;
        --size_;
        return true;
    }

   private:
    void grow() {
        Pool<detail::Slot> old = std::move(slots_);
        const std::size_t n = capacity();
        rebuild(n * 2);
        size_ = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (old[i].key != detail::kEmpty) insert(old[i].key, old[i].val);
    }
};

}  // namespace hft::book
