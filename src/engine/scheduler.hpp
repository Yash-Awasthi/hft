#pragma once

// Discrete-event queue for the backtest. Events pop in (time, kind, insertion) order, so at
// equal timestamps real market events apply before our messages, the conservative choice
// for queue races, and the order is deterministic.

#include <cstdint>
#include <vector>

namespace hft::engine {

// Declaration order is the tie-break at equal time.
enum class Kind : std::uint8_t { Market, MarketData, OrderArrival, Report };

struct Timed {
    std::uint64_t time;  // ns since midnight
    std::uint64_t key;   // kind in the top 8 bits, insertion counter below
    std::uint32_t payload;

    bool before(const Timed& o) const { return time != o.time ? time < o.time : key < o.key; }
    Kind kind() const { return static_cast<Kind>(key >> 56); }
};

class EventQueue {
   public:
    // Capacity is reserved up front; pushing beyond it grows the heap.
    explicit EventQueue(std::size_t capacity) { heap_.reserve(capacity); }

    void push(std::uint64_t time, Kind kind, std::uint32_t payload) {
        heap_.push_back(
            {time, std::uint64_t{static_cast<std::uint8_t>(kind)} << 56 | counter_++, payload});
        std::size_t i = heap_.size() - 1;
        const Timed x = heap_[i];
        while (i > 0) {
            const std::size_t p = (i - 1) / 2;
            if (!x.before(heap_[p])) break;
            heap_[i] = heap_[p];
            i = p;
        }
        heap_[i] = x;
    }

    bool peek(Timed& out) const {
        if (heap_.empty()) return false;
        out = heap_.front();
        return true;
    }

    bool pop(Timed& out) {
        if (heap_.empty()) return false;
        out = heap_.front();
        const Timed x = heap_.back();
        heap_.pop_back();
        const std::size_t n = heap_.size();
        if (n == 0) return true;
        std::size_t i = 0;
        for (;;) {
            std::size_t c = 2 * i + 1;
            if (c >= n) break;
            if (c + 1 < n && heap_[c + 1].before(heap_[c])) ++c;
            if (!heap_[c].before(x)) break;
            heap_[i] = heap_[c];
            i = c;
        }
        heap_[i] = x;
        return true;
    }

    std::size_t size() const { return heap_.size(); }
    bool empty() const { return heap_.empty(); }

   private:
    std::vector<Timed> heap_;
    std::uint64_t counter_ = 0;
};

}  // namespace hft::engine
