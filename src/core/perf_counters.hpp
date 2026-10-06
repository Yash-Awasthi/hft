#pragma once

// Hardware counters for a code region through perf_event_open, one group read per stop.
// Events the kernel or hypervisor does not expose are reported as missing, never estimated.

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace hft {

class PerfCounters {
   public:
    enum Event { Cycles, Instructions, L1dMisses, LlcMisses, BranchMisses, DtlbMisses, kEvents };
    static constexpr std::array<const char*, kEvents> kNames = {
        "cycles", "instructions", "L1d-misses", "LLC-misses", "branch-misses", "dTLB-misses"};

    PerfCounters() {
        for (int e = 0; e < kEvents; ++e) fd_[e] = open(static_cast<Event>(e));
    }
    ~PerfCounters() {
        for (int fd : fd_)
            if (fd >= 0) ::close(fd);
    }
    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    bool available(Event e) const { return fd_[e] >= 0; }
    bool any() const {
        for (int fd : fd_)
            if (fd >= 0) return true;
        return false;
    }

    void start() {
        for (int e = 0; e < kEvents; ++e) begin_[e] = read(e);
    }
    void stop() {
        for (int e = 0; e < kEvents; ++e) total_[e] += read(e) - begin_[e];
    }
    std::uint64_t total(Event e) const { return total_[e]; }

   private:
    static int open(Event e) {
        perf_event_attr a;
        std::memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.type = PERF_TYPE_HARDWARE;
        switch (e) {
            case Cycles:
                a.config = PERF_COUNT_HW_CPU_CYCLES;
                break;
            case Instructions:
                a.config = PERF_COUNT_HW_INSTRUCTIONS;
                break;
            case BranchMisses:
                a.config = PERF_COUNT_HW_BRANCH_MISSES;
                break;
            case L1dMisses:
            case LlcMisses:
            case DtlbMisses:
                a.type = PERF_TYPE_HW_CACHE;
                a.config = (e == L1dMisses   ? PERF_COUNT_HW_CACHE_L1D
                            : e == LlcMisses ? PERF_COUNT_HW_CACHE_LL
                                             : PERF_COUNT_HW_CACHE_DTLB) |
                           (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
                break;
            default:
                return -1;
        }
        return static_cast<int>(::syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
    }
    std::uint64_t read(int e) const {
        std::uint64_t v = 0;
        if (fd_[e] >= 0 && ::read(fd_[e], &v, sizeof v) != sizeof v) v = 0;
        return v;
    }

    std::array<int, kEvents> fd_{};
    std::array<std::uint64_t, kEvents> begin_{}, total_{};
};

}  // namespace hft
