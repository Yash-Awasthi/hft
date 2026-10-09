// Random order sequences applied to every book and checked against the std::map baseline
// after each step: same return value, same BBO, same queue position, invariants intact.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "book/id_map.hpp"
#include "book/level_radix.hpp"
#include "book/map_book.hpp"
#include "book/tick_book.hpp"

using namespace hft::book;

namespace {

struct Op {
    std::uint8_t kind;  // 0-3 add, 4 execute, 5 cancel, 6 delete, 7 replace
    std::uint8_t price_mode;
    bool sell;
    std::uint32_t pick;
    std::uint32_t shares;
    std::uint32_t price;
};

std::uint32_t price_for(const Op& op) {
    switch (op.price_mode % 20) {
        case 0:
            return 1 + op.price % ((1u << 31) - 1);  // anywhere, usually far
        case 1:
            return 100'0000 + op.price % 20000;  // off the penny grid
        case 2:
            return 1 + op.price % 1'2000;  // around and below $1
        case 3:
        case 4:
            return 50'0000 + (op.price % 10000) * 100;  // on the grid, often beyond the window
        default:
            return 100'0000 + (op.price % 101) * 100 - 50 * 100;  // within 50 ticks
    }
}

struct Model {
    std::vector<std::uint64_t> refs;  // live references, in insertion order
    std::uint64_t next_ref = 1;

    std::uint64_t pick(std::uint32_t p) const {
        if (refs.empty() || p % 16 == 0) return 1'000'000 + p;  // unknown reference
        return refs[p % refs.size()];
    }
    void drop(std::uint64_t r) { std::erase(refs, r); }
};

template <class... Books>
void run(const std::vector<Op>& ops) {
    MapBook ref_book;
    std::tuple<Books...> books;
    Model m;
    std::uint64_t seq = 0;
    for (const Op& op : ops) {
        ++seq;
        const Side side = op.sell ? Side::Sell : Side::Buy;
        const std::uint32_t shares = op.shares % 8 == 0 ? 0 : 1 + op.shares % 500;
        const std::uint32_t px = price_for(op);
        auto apply = [&](auto& b) -> bool {
            switch (op.kind % 8) {
                case 0:
                case 1:
                case 2:
                case 3: {
                    const std::uint64_t r = op.pick % 32 == 0 ? m.pick(op.pick / 32) : m.next_ref;
                    return b.add(r, side, shares, px, seq);
                }
                case 4:
                    return b.execute(m.pick(op.pick), shares);
                case 5:
                    return b.cancel(m.pick(op.pick), shares);
                case 6:
                    return b.erase(m.pick(op.pick));
                default:
                    return b.replace(m.pick(op.pick), m.next_ref, shares, px, seq);
            }
        };
        const bool want = apply(ref_book);
        std::apply([&](auto&... b) { ((RC_ASSERT(apply(b) == want)), ...); }, books);
        // Update the model of live references from the baseline book.
        if (want) {
            const std::uint64_t kind = op.kind % 8;
            if (kind < 4 && !(op.pick % 32 == 0)) m.refs.push_back(m.next_ref++);
            if (kind == 7) {
                m.drop(m.pick(op.pick));
                m.refs.push_back(m.next_ref++);
            }
        }
        m.refs.erase(std::remove_if(m.refs.begin(), m.refs.end(),
                                    [&](std::uint64_t r) { return ref_book.queue_ahead(r) < 0; }),
                     m.refs.end());
        const Bbo bbo = ref_book.bbo();
        std::apply(
            [&](auto&... b) {
                ((RC_ASSERT(b.bbo() == bbo)), ...);
                ((RC_ASSERT(b.order_count() == ref_book.order_count())), ...);
                ((RC_ASSERT(b.check())), ...);
                (
                    [&] {
                        if constexpr (requires { b.next_level(Side::Buy, 0u); }) {
                            for (Side sd : {Side::Buy, Side::Sell}) {
                                std::vector<std::pair<std::uint32_t, std::uint64_t>> walk;
                                for (auto f = b.front(sd); f; f = b.next_level(sd, f->price))
                                    walk.emplace_back(f->price, b.level_qty(sd, f->price));
                                RC_ASSERT(walk == ref_book.depth(sd));
                            }
                        }
                    }(),
                    ...);
                if (!m.refs.empty()) {
                    const std::uint64_t r = m.refs[op.pick % m.refs.size()];
                    ((RC_ASSERT(b.queue_ahead(r) == ref_book.queue_ahead(r))), ...);
                }
            },
            books);
    }
    RC_ASSERT(ref_book.check());
}

}  // namespace

namespace rc {
template <>
struct Arbitrary<Op> {
    static Gen<Op> arbitrary() {
        return gen::build<Op>(gen::set(&Op::kind), gen::set(&Op::price_mode), gen::set(&Op::sell),
                              gen::set(&Op::pick), gen::set(&Op::shares), gen::set(&Op::price));
    }
};
}  // namespace rc

RC_GTEST_PROP(BookProp, AllBooksMatchBaseline, (const std::vector<Op>& ops)) {
    run<TickBook>(ops);
}

template <class Map>
void map_matches(const std::vector<std::pair<std::uint8_t, std::uint16_t>>& ops) {
    Map map(4);
    std::unordered_map<std::uint64_t, std::uint32_t> want;
    std::uint32_t v = 0;
    for (const auto& [kind, k] : ops) {
        const std::uint64_t key = k % 96 * 7919;  // dense enough to collide
        if (kind % 3 != 0 && !want.contains(key)) {
            map.insert(key, ++v);
            want[key] = v;
        } else if (kind % 3 == 0) {
            RC_ASSERT(map.erase(key) == (want.erase(key) == 1));
        }
        RC_ASSERT(map.size() == want.size());
        for (std::uint64_t probe = 0; probe < 96; ++probe) {
            const auto it = want.find(probe * 7919);
            RC_ASSERT(map.find(probe * 7919) == (it == want.end() ? kNoOrder : it->second));
        }
    }
    std::size_t n = 0;
    map.for_each([&](std::uint64_t key, std::uint32_t val) {
        RC_ASSERT(want.at(key) == val);
        ++n;
    });
    RC_ASSERT(n == want.size());
}

RC_GTEST_PROP(IdMapProp, LinearMatchesUnorderedMap,
              (const std::vector<std::pair<std::uint8_t, std::uint16_t>>& ops)) {
    map_matches<LinearMap>(ops);
}

// Level radix against std::map: membership, extremes and in-order iteration.
RC_GTEST_PROP(LevelRadixProp, MatchesStdMap,
              (const std::vector<std::pair<bool, std::uint32_t>>& ops)) {
    struct L {
        std::uint64_t qty;
    };
    LevelRadix<L> r;
    std::map<std::uint32_t, std::uint64_t> want;
    for (const auto& [insert, raw] : ops) {
        // Mostly clustered indices, sometimes anywhere in range.
        const std::uint32_t idx = raw % 4 == 0 ? raw % LevelRadix<L>::kRange : 500'000 + raw % 2000;
        if (insert) {
            r.get(idx).qty += 1;
            want[idx] += 1;
        } else if (want.erase(idx)) {
            r.erase(idx);
        }
        RC_ASSERT(r.count() == want.size());
        RC_ASSERT((r.find(idx) != nullptr) == want.contains(idx));
        std::uint32_t nb, na;
        const auto lo_it = want.lower_bound(idx);
        const auto hi_it = want.upper_bound(idx);
        RC_ASSERT(r.next(true, idx, nb) == (lo_it != want.begin()));
        if (lo_it != want.begin()) RC_ASSERT(nb == std::prev(lo_it)->first);
        RC_ASSERT(r.next(false, idx, na) == (hi_it != want.end()));
        if (hi_it != want.end()) RC_ASSERT(na == hi_it->first);
        std::uint32_t lo, hi;
        RC_ASSERT(r.extreme(false, lo) == !want.empty());
        if (!want.empty()) {
            r.extreme(true, hi);
            RC_ASSERT(lo == want.begin()->first);
            RC_ASSERT(hi == want.rbegin()->first);
        }
    }
    auto it = want.begin();
    r.for_each([&](std::uint32_t idx, const L& l) {
        RC_ASSERT(it != want.end() && it->first == idx && it->second == l.qty);
        ++it;
    });
    RC_ASSERT(it == want.end());
}
