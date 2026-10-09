#pragma once

// Order-ID maps from an ITCH reference (below kMaxRef) to an order index (below 2^24).
// Open addressing over a power-of-two table, kept at most half full, with backward-shift
// deletion so no tombstones build up over a day.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "book/order_store.hpp"
#include "book/types.hpp"
#include "core/pool.hpp"

namespace hft::book {

inline constexpr std::uint32_t kNoOrder = 0xffffffffu;

// One lookup that serves the following insert or erase. `at` is opaque to callers; val is
// kNoOrder when the key is absent.
struct IdProbe {
    std::size_t at;
    std::uint32_t val;
};

namespace detail {

// Key (40 bits) and order index (24 bits) packed in 8 bytes; all ones marks an empty slot,
// so the largest key is reserved.
struct Slot {
    std::uint64_t bits;
    static Slot make(std::uint64_t key, std::uint32_t val) { return {key << 24 | val}; }
    std::uint64_t key() const { return bits >> 24; }
    std::uint32_t val() const { return static_cast<std::uint32_t>(bits & 0xffffff); }
    bool empty() const { return bits == ~0ull; }
    void clear() { bits = ~0ull; }
};
static_assert(sizeof(Slot) == 8);

// Shared table storage; Derived supplies insert, find and erase.
class Table {
   public:
    explicit Table(std::size_t expected) {
        rebuild(std::bit_ceil(expected < 32 ? 64 : expected * 2));
    }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return mask_ + 1; }
    void clear() {
        for (std::size_t i = 0; i <= mask_; ++i) slots_[i].clear();
        size_ = 0;
    }
    void prefetch(std::uint64_t key) const { __builtin_prefetch(&slots_[home(key)]); }

    void copy_from(const Table& o) {
        if (capacity() != o.capacity()) rebuild(o.capacity());
        slots_.copy_from(o.slots_, o.capacity());
        size_ = o.size_;
    }

    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i <= mask_; ++i)
            if (!slots_[i].empty()) f(slots_[i].key(), slots_[i].val());
    }

    // Raw table image, so a loaded map probes exactly like the saved one.
    void save(std::vector<std::uint8_t>& out) const {
        const std::uint64_t head[2] = {capacity(), size_};
        put(out, head, sizeof head);
        put(out, slots_.data(), capacity() * sizeof(Slot));
    }
    bool load(const std::uint8_t*& p, const std::uint8_t* end) {
        std::uint64_t head[2];
        if (end - p < static_cast<std::ptrdiff_t>(sizeof head)) return false;
        std::memcpy(head, p, sizeof head);
        if (!std::has_single_bit(head[0]) || head[0] < 64 || head[1] * 2 > head[0]) return false;
        if (static_cast<std::uint64_t>(end - p) - sizeof head < head[0] * sizeof(Slot))
            return false;
        p += sizeof head;
        if (head[0] != capacity()) rebuild(head[0]);
        std::memcpy(slots_.data(), p, head[0] * sizeof(Slot));
        p += head[0] * sizeof(Slot);
        size_ = head[1];
        return true;
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
        for (std::size_t i = 0; i < cap; ++i) slots_[i].clear();
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
            if (s.key() == key) return s.val();
            if (s.empty()) return kNoOrder;
        }
    }

    // The key's slot, or the first empty slot of its probe path.
    IdProbe probe(std::uint64_t key) const {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const detail::Slot& s = slots_[i];
            if (s.key() == key) return {i, s.val()};
            if (s.empty()) return {i, kNoOrder};
        }
    }

    // `key` must be absent.
    void insert(std::uint64_t key, std::uint32_t val) {
        if ((size_ + 1) * 2 > capacity()) grow();
        std::size_t i = home(key);
        while (!slots_[i].empty()) i = (i + 1) & mask_;
        slots_[i] = detail::Slot::make(key, val);
        ++size_;
    }
    // `p` is a probe for the absent `key` with the table unchanged since.
    void insert_at(IdProbe p, std::uint64_t key, std::uint32_t val) {
        if ((size_ + 1) * 2 > capacity()) return insert(key, val);
        slots_[p.at] = detail::Slot::make(key, val);
        ++size_;
    }

    bool erase(std::uint64_t key) {
        const IdProbe p = probe(key);
        if (p.val == kNoOrder) return false;
        erase_at(p.at);
        return true;
    }
    // Erases the entry in slot i; returns the slot left empty.
    std::size_t erase_at(std::size_t i) {
        // Pull back any later entry whose probe path crosses the hole.
        for (std::size_t j = (i + 1) & mask_; !slots_[j].empty(); j = (j + 1) & mask_) {
            if (dist(j, slots_[j].key()) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].clear();
        --size_;
        return i;
    }
    // Probe `p` of an absent key, updated for an erase_at that left `hole` empty. Erasing
    // only shifts entries back within a run, so the hole is the run's single new empty slot.
    IdProbe after_erase(IdProbe p, std::uint64_t key, std::size_t hole) const {
        const std::size_t h = home(key);
        return ((hole - h) & mask_) < ((p.at - h) & mask_) ? IdProbe{hole, kNoOrder} : p;
    }

   private:
    void grow() {
        Pool<detail::Slot> old = std::move(slots_);
        const std::size_t n = capacity();
        rebuild(n * 2);
        size_ = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (!old[i].empty()) insert(old[i].key(), old[i].val());
    }
};

}  // namespace hft::book
