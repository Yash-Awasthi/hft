// Order book study: store read + decode throughput, per-event book update latency and the
// end-to-end replay, over the busiest symbols of a store. Prints one CSV row per repetition;
// research/book_study.py turns them into the comparison table.
// Usage: book_study <store-dir> <mode> [top-n] [reps]
//   mode: decode | book | replay | book-cg:<variant> (one untimed pass, for Cachegrind)

#include <hdr/hdr_histogram.h>

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#endif
#ifndef CACHEGRIND_START_INSTRUMENTATION
#define CACHEGRIND_START_INSTRUMENTATION
#define CACHEGRIND_STOP_INSTRUMENTATION
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "book/btree_book.hpp"
#include "book/itch_apply.hpp"
#include "book/map_book.hpp"
#include "book/tick_book.hpp"
#include "core/pool.hpp"
#include "core/tsc.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft;

std::vector<std::uint16_t> busiest(const std::filesystem::path& dir, std::size_t n) {
    std::vector<std::pair<std::uint64_t, std::uint16_t>> v;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".idx") continue;
        const auto loc = static_cast<std::uint16_t>(std::stoul(e.path().stem().string()));
        if (loc == 0) continue;
        std::uint64_t msgs = 0;
        const data::SymbolReader rd(dir, loc);
        for (const auto& c : rd.index()) msgs += c.n_msgs;
        v.emplace_back(msgs, loc);
    }
    std::sort(v.rbegin(), v.rend());
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < std::min(n, v.size()); ++i) out.push_back(v[i].second);
    std::sort(out.begin(), out.end());
    return out;
}

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// Touches every decoded field so the decode cannot be optimised away.
struct Checksum {
    std::uint64_t h = 0;
    void operator()(const itch::AddOrder& m) { h += m.ref ^ m.price ^ m.shares; }
    void operator()(const itch::OrderExecuted& m) { h += m.ref ^ m.shares; }
    void operator()(const itch::OrderExecutedPrice& m) { h += m.ref ^ m.price; }
    void operator()(const itch::OrderCancel& m) { h += m.ref ^ m.cancelled; }
    void operator()(const itch::OrderDelete& m) { h += m.ref; }
    void operator()(const itch::OrderReplace& m) { h += m.new_ref ^ m.price; }
    template <class T>
    void operator()(const T& m) {
        h += sizeof m;
    }
};

void decode(const std::filesystem::path& dir, const std::vector<std::uint16_t>& locs, int reps) {
    for (int r = 0; r <= reps; ++r) {
        data::MergedReader rd(dir, locs, 64);
        data::Record rec{};
        std::uint16_t loc;
        Checksum cs;
        std::uint64_t n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (rd.next(rec, loc)) {
            itch::dispatch(rec.data, rec.len, cs);
            ++n;
        }
        const double s = secs(t0, std::chrono::steady_clock::now());
        if (r > 0)  // first repetition is warm-up
            std::printf("decode,merged,%d,%llu,%.0f,,,,,%llu\n", r, (unsigned long long)n, s * 1e9,
                        (unsigned long long)(cs.h & 0xffff));
    }
}

struct Event {
    char type;
    book::Side side;
    std::uint16_t sym;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t ref;
    std::uint64_t ref2;
};

struct Collect {
    std::vector<Event>& out;
    std::uint16_t sym = 0;
    void operator()(const itch::AddOrder& m) {
        out.push_back({'A', m.side == 'B' ? book::Side::Buy : book::Side::Sell, sym, m.shares,
                       m.price, m.ref, 0});
    }
    void operator()(const itch::OrderExecuted& m) {
        out.push_back({'E', {}, sym, m.shares, 0, m.ref, 0});
    }
    void operator()(const itch::OrderExecutedPrice& m) {
        out.push_back({'E', {}, sym, m.shares, 0, m.ref, 0});
    }
    void operator()(const itch::OrderCancel& m) {
        out.push_back({'X', {}, sym, m.cancelled, 0, m.ref, 0});
    }
    void operator()(const itch::OrderDelete& m) { out.push_back({'D', {}, sym, 0, 0, m.ref, 0}); }
    void operator()(const itch::OrderReplace& m) {
        out.push_back({'U', {}, sym, m.shares, m.price, m.orig_ref, m.new_ref});
    }
    template <class T>
    void operator()(const T&) {}
};

template <class Book>
inline bool apply(Book& b, const Event& e, std::uint64_t seq) {
    switch (e.type) {
        case 'A':
            return b.add(e.ref, e.side, e.shares, e.price, seq);
        case 'E':
            return b.execute(e.ref, e.shares);
        case 'X':
            return b.cancel(e.ref, e.shares);
        case 'D':
            return b.erase(e.ref);
        default:
            return b.replace(e.ref, e.ref2, e.shares, e.price, seq);
    }
}

template <class Book>
std::vector<std::unique_ptr<Book>> make_books(const std::vector<std::size_t>& cap) {
    std::vector<std::unique_ptr<Book>> v;
    for (std::size_t c : cap) {
        if constexpr (std::is_constructible_v<Book, std::size_t>)
            v.push_back(std::make_unique<Book>(c));
        else
            v.push_back(std::make_unique<Book>());
    }
    return v;
}

// One untimed pass and one pass timing every event, per repetition, on fresh books.
template <class Book>
void book_variant(const char* name, const std::vector<Event>& ev,
                  const std::vector<std::size_t>& cap, int reps, double tpn, std::uint64_t ovh) {
    hdr_histogram* h;
    hdr_init(1, 10'000'000, 3, &h);
    for (int r = 0; r <= reps; ++r) {
        auto books = make_books<Book>(cap);
        std::uint64_t errors = 0;
        const std::uint64_t c0 = tsc::start();
        for (std::size_t i = 0; i < ev.size(); ++i) errors += !apply(*books[ev[i].sym], ev[i], i);
        const std::uint64_t batch = tsc::stop() - c0;

        books = make_books<Book>(cap);
        hdr_reset(h);
        for (std::size_t i = 0; i < ev.size(); ++i) {
            const std::uint64_t a = tsc::start();
            errors += !apply(*books[ev[i].sym], ev[i], i);
            const std::uint64_t t = tsc::stop() - a;
            hdr_record_value(h, static_cast<std::int64_t>(t > ovh ? t - ovh : 0));
        }
        if (r == 0) continue;  // warm-up
        auto ns = [&](double p) {
            return static_cast<double>(hdr_value_at_percentile(h, p)) / tpn;
        };
        std::printf("book,%s,%d,%zu,%.0f,%.1f,%.1f,%.1f,%.1f,%llu\n", name, r, ev.size(),
                    static_cast<double>(batch) / tpn, ns(50), ns(99), ns(99.9),
                    static_cast<double>(hdr_max(h)) / tpn, (unsigned long long)errors);
    }
    hdr_close(h);
}

template <class Book>
void book_cachegrind(const std::vector<Event>& ev, const std::vector<std::size_t>& cap) {
    auto books = make_books<Book>(cap);
    std::uint64_t errors = 0;
    CACHEGRIND_START_INSTRUMENTATION;
    for (std::size_t i = 0; i < ev.size(); ++i) errors += !apply(*books[ev[i].sym], ev[i], i);
    CACHEGRIND_STOP_INSTRUMENTATION;
    std::printf("events %zu errors %llu\n", ev.size(), (unsigned long long)errors);
}

void book_mode(const std::filesystem::path& dir, const std::vector<std::uint16_t>& locs, int reps,
               std::string_view only) {
    std::vector<Event> ev;
    std::vector<std::size_t> cap(locs.size());
    {
        data::MergedReader rd(dir, locs, 64);
        data::Record rec{};
        std::uint16_t loc;
        Collect c{ev};
        while (rd.next(rec, loc)) {
            c.sym = static_cast<std::uint16_t>(std::lower_bound(locs.begin(), locs.end(), loc) -
                                               locs.begin());
            itch::dispatch(rec.data, rec.len, c);
        }
        // Capacity from a counting pass, so no pool grows inside the measured loop.
        std::vector<book::MapBook> probe(locs.size());
        for (std::size_t i = 0; i < ev.size(); ++i) {
            apply(probe[ev[i].sym], ev[i], i);
            cap[ev[i].sym] = std::max(cap[ev[i].sym], probe[ev[i].sym].order_count());
        }
        for (auto& c2 : cap) c2 = c2 + c2 / 4 + 64;
    }
    using Tick = book::TickBook<book::LinearMap>;
    using TickRh = book::TickBook<book::RobinHoodMap>;
    using TickDm = book::TickBook<book::DirectMap<>>;
    using TickAos = book::TickBook<book::LinearMap, book::Aos>;
    using TickSoa = book::TickBook<book::LinearMap, book::Soa>;
    using SortedVec = book::SortedVecBook<>;
    using BTree = book::BTreeBook<>;
    if (only.starts_with("book-cg:")) {
        const std::string_view v = only.substr(8);
        if (v == "map") book_cachegrind<book::MapBook>(ev, cap);
        if (v == "tick") book_cachegrind<Tick>(ev, cap);
        if (v == "tick-rh") book_cachegrind<TickRh>(ev, cap);
        if (v == "tick-dm") book_cachegrind<TickDm>(ev, cap);
        if (v == "tick-aos") book_cachegrind<TickAos>(ev, cap);
        if (v == "tick-soa") book_cachegrind<TickSoa>(ev, cap);
        if (v == "tick-sv") book_cachegrind<SortedVec>(ev, cap);
        if (v == "btree") book_cachegrind<BTree>(ev, cap);
        if (v == "tick-4k") {
            PoolStats::huge_pages = false;
            book_cachegrind<Tick>(ev, cap);
        }
        return;
    }
    const double tpn = tsc::ticks_per_ns();
    const std::uint64_t ovh = tsc::overhead();
    std::fprintf(stderr, "events %zu, %.4f ticks/ns, timer overhead %llu ticks, invariant tsc %d\n",
                 ev.size(), tpn, (unsigned long long)ovh, tsc::invariant());
    book_variant<book::MapBook>("map", ev, cap, reps, tpn, ovh);
    book_variant<Tick>("tick", ev, cap, reps, tpn, ovh);
    book_variant<TickRh>("tick-rh", ev, cap, reps, tpn, ovh);
    book_variant<TickDm>("tick-dm", ev, cap, reps, tpn, ovh);
    book_variant<TickAos>("tick-aos", ev, cap, reps, tpn, ovh);
    book_variant<TickSoa>("tick-soa", ev, cap, reps, tpn, ovh);
    book_variant<SortedVec>("tick-sv", ev, cap, reps, tpn, ovh);
    book_variant<BTree>("btree", ev, cap, reps, tpn, ovh);
    PoolStats::huge_pages = false;
    book_variant<Tick>("tick-4k", ev, cap, reps, tpn, ovh);
    PoolStats::huge_pages = true;
}

// End to end on one replay thread plus the read-ahead thread: read, decode, apply, BBO.
void replay(const std::filesystem::path& dir, const std::vector<std::uint16_t>& locs, int reps) {
    using Tick = book::TickBook<book::LinearMap>;
    for (int r = 0; r <= reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::unique_ptr<Tick>> books;
        std::vector<book::ItchApply<Tick>> ap;
        for (std::size_t i = 0; i < locs.size(); ++i)
            books.push_back(std::make_unique<Tick>(1 << 16));
        for (auto& b : books) ap.push_back({*b});
        data::MergedReader rd(dir, locs, 64);
        data::Record rec{};
        std::uint16_t loc;
        std::uint64_t n = 0, bbo = 0, errors = 0;
        while (rd.next(rec, loc)) {
            const auto s = static_cast<std::size_t>(
                std::lower_bound(locs.begin(), locs.end(), loc) - locs.begin());
            ap[s].seq = rec.seq;
            itch::dispatch(rec.data, rec.len, ap[s]);
            bbo += books[s]->bbo().bid_px;
            ++n;
        }
        for (auto& a : ap) errors += a.stats.errors;
        const double s = secs(t0, std::chrono::steady_clock::now());
        if (r > 0)
            std::printf("replay,tick,%d,%llu,%.0f,,,,,%llu\n", r, (unsigned long long)n, s * 1e9,
                        (unsigned long long)errors + (bbo & 0));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: book_study <store-dir> <mode> [top-n] [reps]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const std::string_view mode = argv[2];
    const std::size_t top = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 50;
    const int reps = argc > 4 ? std::atoi(argv[4]) : 10;
    const std::vector<std::uint16_t> locs = busiest(dir, top);
    std::printf("mode,variant,rep,events,total_ns,p50_ns,p99_ns,p999_ns,max_ns,check\n");
    if (mode == "decode")
        decode(dir, locs, reps);
    else if (mode == "book" || mode.starts_with("book-cg:"))
        book_mode(dir, locs, reps, mode);
    else if (mode == "replay")
        replay(dir, locs, reps);
    else
        return 2;
    return 0;
}
