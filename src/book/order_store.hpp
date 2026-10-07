#pragma once

// Order storage layouts for TickBook, all index-addressed. HotCold keeps the fields touched
// on every update (price, quantity, queue links) in 16 bytes and the reference and arrival
// sequence apart; Aos keeps one 32-byte record; Soa keeps one array per field.

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
    void prefetch(std::uint32_t i) const { __builtin_prefetch(&hot_[i]); }
    void set(std::uint32_t i, std::uint32_t px, std::uint32_t qty, std::uint32_t next,
             std::uint32_t prev, std::uint64_t ref, std::uint64_t seq) {
        hot_[i] = {px, qty, next, prev};
        cold_[i] = {ref, seq};
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
    };
    Pool<Hot> hot_;
    Pool<Cold> cold_;
};

class Aos {
   public:
    explicit Aos(std::size_t n) : o_(n) {}
    void reserve(std::size_t n) { o_.reserve(n); }
    std::uint32_t& px(std::uint32_t i) { return o_[i].px; }
    std::uint32_t& qty(std::uint32_t i) { return o_[i].qty; }
    std::uint32_t& next(std::uint32_t i) { return o_[i].next; }
    std::uint32_t& prev(std::uint32_t i) { return o_[i].prev; }
    std::uint32_t px(std::uint32_t i) const { return o_[i].px; }
    std::uint32_t qty(std::uint32_t i) const { return o_[i].qty; }
    std::uint32_t next(std::uint32_t i) const { return o_[i].next; }
    std::uint32_t prev(std::uint32_t i) const { return o_[i].prev; }
    std::uint64_t ref(std::uint32_t i) const { return o_[i].ref; }
    void prefetch(std::uint32_t i) const { __builtin_prefetch(&o_[i]); }
    void set(std::uint32_t i, std::uint32_t px, std::uint32_t qty, std::uint32_t next,
             std::uint32_t prev, std::uint64_t ref, std::uint64_t seq) {
        o_[i] = {px, qty, next, prev, ref, seq};
    }
    void copy_from(const Aos& o, std::size_t used) { o_.copy_from(o.o_, used); }
    void save(std::vector<std::uint8_t>& out, std::size_t used) const {
        detail::put(out, o_.data(), used * sizeof(Order));
    }
    bool load(const std::uint8_t*& p, const std::uint8_t* end, std::size_t used) {
        reserve(used);
        return detail::get(p, end, o_.data(), used * sizeof(Order));
    }

   private:
    struct Order {
        std::uint32_t px, qty, next, prev;
        std::uint64_t ref, seq;
    };
    static_assert(sizeof(Order) == 32);
    Pool<Order> o_;
};

class Soa {
   public:
    explicit Soa(std::size_t n) : px_(n), qty_(n), next_(n), prev_(n), ref_(n), seq_(n) {}
    void reserve(std::size_t n) {
        px_.reserve(n);
        qty_.reserve(n);
        next_.reserve(n);
        prev_.reserve(n);
        ref_.reserve(n);
        seq_.reserve(n);
    }
    std::uint32_t& px(std::uint32_t i) { return px_[i]; }
    std::uint32_t& qty(std::uint32_t i) { return qty_[i]; }
    std::uint32_t& next(std::uint32_t i) { return next_[i]; }
    std::uint32_t& prev(std::uint32_t i) { return prev_[i]; }
    std::uint32_t px(std::uint32_t i) const { return px_[i]; }
    std::uint32_t qty(std::uint32_t i) const { return qty_[i]; }
    std::uint32_t next(std::uint32_t i) const { return next_[i]; }
    std::uint32_t prev(std::uint32_t i) const { return prev_[i]; }
    std::uint64_t ref(std::uint32_t i) const { return ref_[i]; }
    void prefetch(std::uint32_t i) const {
        __builtin_prefetch(&px_[i]);
        __builtin_prefetch(&qty_[i]);
        __builtin_prefetch(&next_[i]);
        __builtin_prefetch(&prev_[i]);
    }
    void set(std::uint32_t i, std::uint32_t px, std::uint32_t qty, std::uint32_t next,
             std::uint32_t prev, std::uint64_t ref, std::uint64_t seq) {
        px_[i] = px;
        qty_[i] = qty;
        next_[i] = next;
        prev_[i] = prev;
        ref_[i] = ref;
        seq_[i] = seq;
    }
    void copy_from(const Soa& o, std::size_t used) {
        px_.copy_from(o.px_, used);
        qty_.copy_from(o.qty_, used);
        next_.copy_from(o.next_, used);
        prev_.copy_from(o.prev_, used);
        ref_.copy_from(o.ref_, used);
        seq_.copy_from(o.seq_, used);
    }
    void save(std::vector<std::uint8_t>& out, std::size_t used) const {
        detail::put(out, px_.data(), used * 4);
        detail::put(out, qty_.data(), used * 4);
        detail::put(out, next_.data(), used * 4);
        detail::put(out, prev_.data(), used * 4);
        detail::put(out, ref_.data(), used * 8);
        detail::put(out, seq_.data(), used * 8);
    }
    bool load(const std::uint8_t*& p, const std::uint8_t* end, std::size_t used) {
        reserve(used);
        return detail::get(p, end, px_.data(), used * 4) &&
               detail::get(p, end, qty_.data(), used * 4) &&
               detail::get(p, end, next_.data(), used * 4) &&
               detail::get(p, end, prev_.data(), used * 4) &&
               detail::get(p, end, ref_.data(), used * 8) &&
               detail::get(p, end, seq_.data(), used * 8);
    }

   private:
    Pool<std::uint32_t> px_, qty_, next_, prev_;
    Pool<std::uint64_t> ref_, seq_;
};

}  // namespace hft::book
