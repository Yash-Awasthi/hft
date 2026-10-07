#pragma once

// Price levels of one book side keyed by tick index (< 2^24), in a three-level radix tree
// of 256-way nodes: a fixed top, mid nodes and pages of 256 levels, each with a bitmap of
// non-empty children, so the best level is a few lzcnt/tzcnt and every node is reached by
// index, never by pointer. Pages and mid nodes are freed when they empty.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "book/order_store.hpp"
#include "core/pool.hpp"

namespace hft::book {

template <class Level>
class LevelRadix {
   public:
    static constexpr std::uint32_t kRange = 1u << 24;
    static constexpr std::uint32_t kNone = 0xffffffffu;

    LevelRadix() : mids_(4), pages_(8) { clear(); }

    void clear() {
        std::memset(top_bits_, 0, sizeof top_bits_);
        for (auto& t : top_) t = kNone;
        free_mid_ = free_page_ = kNone;
        used_mids_ = used_pages_ = 0;
        count_ = 0;
    }

    std::size_t count() const { return count_; }

    Level* find(std::uint32_t idx) {
        const std::uint32_t m = top_[idx >> 16];
        if (m == kNone) return nullptr;
        const std::uint32_t p = mids_[m].child[(idx >> 8) & 255];
        if (p == kNone) return nullptr;
        Page& pg = pages_[p];
        const std::uint32_t o = idx & 255;
        return (pg.bits[o / 64] >> (o % 64)) & 1 ? &pg.lv[o] : nullptr;
    }

    const Level* find(std::uint32_t idx) const { return const_cast<LevelRadix*>(this)->find(idx); }

    // Returns the level, creating an empty one if absent.
    Level& get(std::uint32_t idx) {
        const std::uint32_t t = idx >> 16, mi = (idx >> 8) & 255, o = idx & 255;
        if (top_[t] == kNone) {
            top_[t] = alloc_mid();
            top_bits_[t / 64] |= 1ull << (t % 64);
        }
        if (mids_[top_[t]].child[mi] == kNone) {
            const std::uint32_t p = alloc_page();
            Mid& m = mids_[top_[t]];
            m.child[mi] = p;
            m.bits[mi / 64] |= 1ull << (mi % 64);
        }
        Page& pg = pages_[mids_[top_[t]].child[mi]];
        if (!((pg.bits[o / 64] >> (o % 64)) & 1)) {
            pg.bits[o / 64] |= 1ull << (o % 64);
            pg.lv[o] = Level{};
            ++count_;
        }
        return pg.lv[o];
    }

    void erase(std::uint32_t idx) {
        const std::uint32_t t = idx >> 16, mi = (idx >> 8) & 255, o = idx & 255;
        const std::uint32_t m = top_[t];
        const std::uint32_t p = mids_[m].child[mi];
        Page& pg = pages_[p];
        pg.bits[o / 64] &= ~(1ull << (o % 64));
        --count_;
        if (any(pg.bits)) return;
        pg.next_free = free_page_;
        free_page_ = p;
        Mid& md = mids_[m];
        md.child[mi] = kNone;
        md.bits[mi / 64] &= ~(1ull << (mi % 64));
        if (any(md.bits)) return;
        md.next_free = free_mid_;
        free_mid_ = m;
        top_[t] = kNone;
        top_bits_[t / 64] &= ~(1ull << (t % 64));
    }

    // Highest (want_max) or lowest index present; false when empty.
    bool extreme(bool want_max, std::uint32_t& idx) const {
        if (!count_) return false;
        const std::uint32_t t = pick(top_bits_, want_max);
        const Mid& m = mids_[top_[t]];
        const std::uint32_t mi = pick(m.bits, want_max);
        const Page& pg = pages_[m.child[mi]];
        idx = (t << 16) | (mi << 8) | pick(pg.bits, want_max);
        return true;
    }

    // Nearest present index strictly below (down) or above idx; false when none.
    bool next(bool down, std::uint32_t idx, std::uint32_t& out) const {
        if (!count_) return false;
        const std::uint32_t t = idx >> 16, mi = (idx >> 8) & 255, o = idx & 255;
        std::uint32_t k;
        if (top_[t] != kNone) {
            const Mid& m = mids_[top_[t]];
            if (m.child[mi] != kNone && scan(pages_[m.child[mi]].bits, o, down, k)) {
                out = (t << 16) | (mi << 8) | k;
                return true;
            }
            if (scan(m.bits, mi, down, k)) {
                out = (t << 16) | (k << 8) | pick(pages_[m.child[k]].bits, down);
                return true;
            }
        }
        if (!scan(top_bits_, t, down, k)) return false;
        const Mid& m = mids_[top_[k]];
        const std::uint32_t j = pick(m.bits, down);
        out = (k << 16) | (j << 8) | pick(pages_[m.child[j]].bits, down);
        return true;
    }

    const Level& at(std::uint32_t idx) const {
        return pages_[mids_[top_[idx >> 16]].child[(idx >> 8) & 255]].lv[idx & 255];
    }

    template <class F>
    void for_each(F&& f) const {
        for_bits(top_bits_, [&](std::uint32_t t) {
            const Mid& m = mids_[top_[t]];
            for_bits(m.bits, [&](std::uint32_t mi) {
                const Page& pg = pages_[m.child[mi]];
                for_bits(pg.bits, [&](std::uint32_t o) { f((t << 16) | (mi << 8) | o, pg.lv[o]); });
            });
        });
    }

    void copy_from(const LevelRadix& o) {
        std::memcpy(top_, o.top_, sizeof top_);
        std::memcpy(top_bits_, o.top_bits_, sizeof top_bits_);
        mids_.copy_from(o.mids_, o.used_mids_);
        pages_.copy_from(o.pages_, o.used_pages_);
        free_mid_ = o.free_mid_;
        free_page_ = o.free_page_;
        used_mids_ = o.used_mids_;
        used_pages_ = o.used_pages_;
        count_ = o.count_;
    }

    void save(std::vector<std::uint8_t>& out) const {
        const std::uint32_t head[5] = {free_mid_, free_page_, used_mids_, used_pages_,
                                       static_cast<std::uint32_t>(count_)};
        detail::put(out, head, sizeof head);
        detail::put(out, top_, sizeof top_);
        detail::put(out, top_bits_, sizeof top_bits_);
        detail::put(out, mids_.data(), used_mids_ * sizeof(Mid));
        detail::put(out, pages_.data(), used_pages_ * sizeof(Page));
    }
    bool load(const std::uint8_t*& p, const std::uint8_t* end) {
        std::uint32_t head[5];
        if (!detail::get(p, end, head, sizeof head)) return false;
        free_mid_ = head[0];
        free_page_ = head[1];
        used_mids_ = head[2];
        used_pages_ = head[3];
        count_ = head[4];
        mids_.reserve(used_mids_);
        pages_.reserve(used_pages_);
        return detail::get(p, end, top_, sizeof top_) &&
               detail::get(p, end, top_bits_, sizeof top_bits_) &&
               detail::get(p, end, mids_.data(), used_mids_ * sizeof(Mid)) &&
               detail::get(p, end, pages_.data(), used_pages_ * sizeof(Page));
    }

   private:
    struct Mid {
        std::uint64_t bits[4];
        std::uint32_t child[256];
        std::uint32_t next_free;
        std::uint32_t pad;
    };
    struct Page {
        std::uint64_t bits[4];
        Level lv[256];
        std::uint32_t next_free;
        std::uint32_t pad;
    };

    static bool any(const std::uint64_t (&b)[4]) { return (b[0] | b[1] | b[2] | b[3]) != 0; }
    static std::uint32_t pick(const std::uint64_t (&b)[4], bool want_max) {
        if (want_max) {
            for (int w = 3;; --w)
                if (b[w]) return static_cast<std::uint32_t>(w * 64 + 63 - std::countl_zero(b[w]));
        }
        for (int w = 0;; ++w)
            if (b[w]) return static_cast<std::uint32_t>(w * 64 + std::countr_zero(b[w]));
    }
    // Highest set bit strictly below pos (down) or lowest strictly above it.
    static bool scan(const std::uint64_t (&b)[4], std::uint32_t pos, bool down,
                     std::uint32_t& out) {
        const std::uint32_t w = pos / 64, bit = pos % 64;
        if (down) {
            std::uint64_t x = b[w] & ((1ull << bit) - 1);
            for (int i = static_cast<int>(w);; x = b[--i]) {
                if (x) {
                    out = static_cast<std::uint32_t>(i * 64 + 63 - std::countl_zero(x));
                    return true;
                }
                if (i == 0) return false;
            }
        }
        std::uint64_t x = bit == 63 ? 0 : b[w] & ~((2ull << bit) - 1);
        for (std::uint32_t i = w;; x = b[++i]) {
            if (x) {
                out = i * 64 + static_cast<std::uint32_t>(std::countr_zero(x));
                return true;
            }
            if (i == 3) return false;
        }
    }

    template <class F>
    static void for_bits(const std::uint64_t (&b)[4], F&& f) {
        for (std::uint32_t w = 0; w < 4; ++w)
            for (std::uint64_t x = b[w]; x; x &= x - 1) f(w * 64 + std::countr_zero(x));
    }

    std::uint32_t alloc_mid() {
        std::uint32_t m;
        if (free_mid_ != kNone) {
            m = free_mid_;
            free_mid_ = mids_[m].next_free;
        } else {
            m = used_mids_++;
            mids_.reserve(used_mids_);
        }
        std::memset(mids_[m].bits, 0, sizeof mids_[m].bits);
        for (auto& c : mids_[m].child) c = kNone;
        return m;
    }
    std::uint32_t alloc_page() {
        std::uint32_t p;
        if (free_page_ != kNone) {
            p = free_page_;
            free_page_ = pages_[p].next_free;
        } else {
            p = used_pages_++;
            pages_.reserve(used_pages_);
        }
        std::memset(pages_[p].bits, 0, sizeof pages_[p].bits);
        return p;
    }

    std::uint32_t top_[256];
    std::uint64_t top_bits_[4];
    Pool<Mid> mids_;
    Pool<Page> pages_;
    std::uint32_t free_mid_, free_page_, used_mids_, used_pages_;
    std::size_t count_;
};

}  // namespace hft::book
