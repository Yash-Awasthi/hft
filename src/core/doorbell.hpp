#pragma once

// Wakes a consumer that sleeps when its queues are empty, without a syscall on the producer
// side while the consumer is awake. Producer: publish, then ring(). Consumer: snapshot(),
// re-check the queues, then wait(snapshot, timeout). The sequence counter and the waiting
// flag are both seq_cst, so either the producer sees the flag or the consumer sees the new
// sequence (a lost wake-up needs both to miss).

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <ctime>

namespace hft {

class Doorbell {
   public:
    void ring() {
        seq_.fetch_add(1);
        if (waiting_.load()) futex(FUTEX_WAKE_PRIVATE, 1, nullptr);
    }
    std::uint32_t snapshot() const { return seq_.load(); }
    // Sleeps until rung after `seen` was taken, or for at most timeout_us.
    void wait(std::uint32_t seen, long timeout_us) {
        waiting_.store(true);
        if (seq_.load() == seen) {
            const timespec ts{timeout_us / 1'000'000, (timeout_us % 1'000'000) * 1000};
            futex(FUTEX_WAIT_PRIVATE, seen, &ts);
        }
        waiting_.store(false);
    }

   private:
    long futex(int op, std::uint32_t val, const timespec* ts) {
        return ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&seq_), op, val, ts, nullptr, 0);
    }
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<bool> waiting_{false};
};

}  // namespace hft
