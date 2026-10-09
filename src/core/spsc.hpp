#pragma once

// Bounded single-producer single-consumer ring. Each side owns one index on its own cache
// line and keeps a cached copy of the other side's, so the shared line is read only when
// the ring looks full (producer) or empty (consumer).

#include <immintrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace hft {

template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

   public:
    bool try_push(T v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;
        }
        slots_[t & (N - 1)] = std::move(v);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = std::move(slots_[h & (N - 1)]);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Blocking forms: spin with pause, then yield the core.
    void push(T v) {
        for (int spins = 0; !try_push(v); ++spins) wait(spins);
    }
    T pop() {
        T v;
        for (int spins = 0; !try_pop(v); ++spins) wait(spins);
        return v;
    }

   private:
    static void wait(int spins) {
        if (spins < 64) _mm_pause();
        else std::this_thread::yield();
    }

    alignas(64) std::atomic<std::size_t> head_{0};  // next slot to read; written by the consumer
    std::size_t tail_cache_ = 0;                     // consumer's view of tail_
    alignas(64) std::atomic<std::size_t> tail_{0};  // next slot to write; written by the producer
    std::size_t head_cache_ = 0;                     // producer's view of head_
    alignas(64) T slots_[N]{};
};

// Single-producer single-consumer ring of variable-length records: a fixed header and the
// payload, 8-byte aligned, written in place. A record that would straddle the end of the
// buffer is preceded by a wrap marker and starts again at offset 0, so every payload is
// contiguous and the consumer reads it without a copy.
class SpscBytes {
   public:
    struct Rec {
        std::int64_t ns;
        std::uint64_t tsc;
        std::uint32_t tag;
        std::string_view data;  // valid until release()
    };

    explicit SpscBytes(std::size_t capacity_pow2)
        : cap_(capacity_pow2), buf_(new (std::align_val_t(64)) std::byte[capacity_pow2]) {}
    ~SpscBytes() { ::operator delete[](buf_, std::align_val_t(64)); }
    SpscBytes(const SpscBytes&) = delete;
    SpscBytes& operator=(const SpscBytes&) = delete;

    std::size_t max_payload() const { return cap_ / 2 - kHead; }

    // False when full; payloads above max_payload() never fit.
    bool try_write(std::int64_t ns, std::uint64_t tsc, std::uint32_t tag, std::string_view data) {
        if (data.size() > max_payload()) return false;
        const std::size_t need = round8(kHead + data.size());
        std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t off = t & (cap_ - 1);
        const std::size_t contig = cap_ - off;
        const std::size_t total = contig < need ? contig + need : need;
        if (t + total - head_cache_ > cap_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t + total - head_cache_ > cap_) return false;
        }
        if (contig < need) {
            const std::uint32_t wrap = kWrap;
            std::memcpy(buf_ + off, &wrap, sizeof wrap);
            t += contig, off = 0;
        }
        const Header h{static_cast<std::uint32_t>(data.size()), tag, ns, tsc};
        std::memcpy(buf_ + off, &h, sizeof h);
        std::memcpy(buf_ + off + kHead, data.data(), data.size());
        tail_.store(t + need, std::memory_order_release);
        return true;
    }

    // The oldest record, left in place until release().
    bool try_read(Rec& r) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        std::size_t off = h & (cap_ - 1);
        std::uint32_t len;
        std::memcpy(&len, buf_ + off, sizeof len);
        if (len == kWrap) h += cap_ - off, off = 0;  // the record after a marker is published with it
        Header hd;
        std::memcpy(&hd, buf_ + off, sizeof hd);
        r = {hd.ns, hd.tsc, hd.tag, {reinterpret_cast<const char*>(buf_ + off + kHead), hd.len}};
        next_ = h + round8(kHead + hd.len);
        return true;
    }
    void release() { head_.store(next_, std::memory_order_release); }

   private:
    struct Header {
        std::uint32_t len, tag;
        std::int64_t ns;
        std::uint64_t tsc;
    };
    static constexpr std::size_t kHead = sizeof(Header);
    static constexpr std::uint32_t kWrap = ~0u;
    static std::size_t round8(std::size_t n) { return (n + 7) & ~std::size_t{7}; }

    const std::size_t cap_;
    std::byte* const buf_;
    alignas(64) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0, next_ = 0;
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
};

}  // namespace hft
