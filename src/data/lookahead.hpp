#pragma once

// Delays a MergedReader by N messages so the consumer can prefetch what the next N will touch.
// The merged stream is already in memory a window ahead; this keeps copies of the next N
// records and hands each one back to a callback the moment it is read, then returns them
// in order. Replay is bound by cache misses on the order-ID table, which the prefetches hide.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "data/store.hpp"

namespace hft::data {

template <std::size_t N = 16>
class Lookahead {
    static_assert(N >= 1 && N <= 256);

   public:
    explicit Lookahead(MergedReader& rd) : rd_(rd) {}

    // `warm(locate, data, len)` runs once per record as it enters the window and returns a
    // tag that comes back with the record. `late(tag, data, len)` runs when the record is
    // kLate positions from the front.
    template <class Warm, class Late>
    bool next(Record& out, std::uint16_t& locate, std::uint32_t& tag, Warm&& warm, Late&& late) {
        while (count_ < N && !eof_) {
            Item& it = items_[(head_ + count_) % (N + 1)];
            Record r;
            if (!rd_.next(r, it.locate)) {
                eof_ = true;
                break;
            }
            it.seq = r.seq;
            it.len = r.len;
            std::memcpy(it.msg, r.data, r.len);
            it.tag = warm(it.locate, it.msg, it.len);
            ++count_;
        }
        if (count_ == 0) return false;
        if (count_ > kLate) {
            const Item& l = items_[(head_ + kLate) % (N + 1)];
            late(l.tag, l.msg, l.len);
        }
        const Item& it = items_[head_];
        out = {it.seq, it.msg, it.len};
        locate = it.locate;
        tag = it.tag;
        head_ = (head_ + 1) % (N + 1);
        --count_;
        return true;
    }

    static constexpr std::size_t kLate = N / 2;

   private:
    struct Item {
        std::uint64_t seq;
        std::uint32_t tag;
        std::uint16_t locate, len;
        std::uint8_t msg[56];  // the longest ITCH 5.0 message is 50 bytes
    };
    MergedReader& rd_;
    std::array<Item, N + 1> items_{};
    std::size_t head_ = 0, count_ = 0;
    bool eof_ = false;
};

// Order references an ITCH message names: the one it acts on, and for a replace the new one.
// Zero when the message has none.
struct ItchRefs {
    std::uint64_t first = 0, second = 0;
};
inline ItchRefs itch_refs(const std::uint8_t* m, std::size_t len) {
    ItchRefs r;
    if (len < 19) return r;
    auto be64 = [](const std::uint8_t* p) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        return __builtin_bswap64(v);
    };
    switch (m[0]) {
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D':
            r.first = be64(m + 11);
            break;
        case 'U':
            r.first = be64(m + 11);
            if (len >= 27) r.second = be64(m + 19);
            break;
        default: break;
    }
    return r;
}

}  // namespace hft::data
