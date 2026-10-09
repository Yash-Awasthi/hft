#pragma once

// Bounded single-producer single-consumer ring. Each side owns one index on its own cache
// line and keeps a cached copy of the other side's, so the shared line is read only when
// the ring looks full (producer) or empty (consumer).

#include <immintrin.h>

#include <atomic>
#include <cstddef>
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

}  // namespace hft
