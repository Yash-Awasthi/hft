// Replays symbols of a store through a book and reports per-symbol book statistics, a hash
// of the BBO stream and invariant failures.
// Usage: book_replay <store-dir> <book> [locate ...]   (no locate: every symbol)
//   book: map | tick | tick-rh | tick-dm | tick-aos | tick-soa | tick-sv | btree
// Env: CHECK_EVERY=N runs the full invariant check every N book events (default 0: off).
//      THREADS=N replays N symbols at a time (default: all hardware threads); rows print in locate order.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "book/btree_book.hpp"
#include "book/itch_apply.hpp"
#include "book/map_book.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft;

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
};

struct Result {
    std::string symbol;
    std::uint64_t msgs = 0, bbo_changes = 0, crossed = 0, locked = 0, max_orders = 0;
    std::uint64_t max_cross_ns = 0, crossed_trading = 0, check_failures = 0;
    book::ApplyStats stats;
    Fnv hash;
};

// The longest cross in trading state on 2025-11-28 lasted 1.4 ms (post-halt uncrossing).
constexpr std::uint64_t kPersistentCrossNs = 100'000'000;

struct SymbolName {
    std::string& out;
    char& state;
    void operator()(const itch::TradingAction& m) { state = m.state; }
    void operator()(const itch::StockDirectory& m) {
        out.assign(m.stock, 8);
        out.erase(out.find_last_not_of(' ') + 1);
    }
    template <class T>
    void operator()(const T&) {}
};

// Share flow into and out of the book, read before each message is applied, so that
// added = executed + cancelled + deleted + replaced + resting at any point.
template <class Book>
struct Flow {
    const Book& b;
    std::uint64_t added = 0, cancelled = 0, removed = 0;
    void operator()(const itch::AddOrder& m) { added += m.shares; }
    void operator()(const itch::OrderCancel& m) { cancelled += m.cancelled; }
    void operator()(const itch::OrderDelete& m) { removed += b.shares(m.ref); }
    void operator()(const itch::OrderReplace& m) {
        removed += b.shares(m.orig_ref);
        added += m.shares;
    }
    template <class T>
    void operator()(const T&) {}
};

template <class Book>
Result replay(const std::filesystem::path& dir, std::uint16_t locate, std::uint64_t check_every) {
    Book b;
    book::ItchApply<Book> ap{b};
    Result r;
    data::SymbolReader rd(dir, locate);
    data::Record rec{};
    book::Bbo last{};
    std::uint64_t cross_start = 0;
    bool in_cross = false;
    char state = 0;
    SymbolName sym{r.symbol, state};
    Flow<Book> flow{b};
    while (rd.next(rec)) {
        ++r.msgs;
        itch::dispatch(rec.data, rec.len, flow);
        ap.seq = rec.seq;
        const std::uint64_t before = ap.stats.book_msgs;
        itch::dispatch(rec.data, rec.len, ap);
        if (rec.data[0] == 'R' || rec.data[0] == 'H') itch::dispatch(rec.data, rec.len, sym);
        // A cross is persistent when it outlives the uncrossing burst after a halt.
        const book::Bbo cur = b.bbo();
        const bool both = cur.bid_px && cur.ask_px;
        if (both && cur.bid_px > cur.ask_px && state == 'T') {
            const std::uint64_t ts = itch::detail::read_header(rec.data).timestamp;
            if (!in_cross) cross_start = ts, in_cross = true;
            r.max_cross_ns = std::max(r.max_cross_ns, ts - cross_start);
        } else {
            in_cross = false;
        }
        if (ap.stats.book_msgs == before) continue;
        if (check_every && ap.stats.book_msgs % check_every == 0 && !b.check()) ++r.check_failures;
        r.max_orders = std::max<std::uint64_t>(r.max_orders, b.order_count());
        if (cur == last) continue;
        last = cur;
        ++r.bbo_changes;
        r.hash.add(&rec.seq, 8);
        r.hash.add(&cur, sizeof cur);
        r.crossed += both && cur.bid_px > cur.ask_px;
        r.crossed_trading += both && cur.bid_px > cur.ask_px && state == 'T';
        r.locked += both && cur.bid_px == cur.ask_px;
    }
    if (check_every && !b.check()) ++r.check_failures;
    if (flow.added != ap.stats.executed + flow.cancelled + flow.removed + b.resting_shares())
        ++r.check_failures;
    r.stats = ap.stats;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: book_replay <store-dir> <book> [locate ...]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const std::string_view kind = argv[2];
    const char* ce = std::getenv("CHECK_EVERY");
    const std::uint64_t check_every = ce ? std::strtoull(ce, nullptr, 10) : 0;

    std::vector<std::uint16_t> locates;
    for (int i = 3; i < argc; ++i)
        locates.push_back(static_cast<std::uint16_t>(std::atoi(argv[i])));
    if (locates.empty())
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".idx")
                locates.push_back(static_cast<std::uint16_t>(std::stoul(e.path().stem().string())));
    std::sort(locates.begin(), locates.end());

    std::printf(
        "locate symbol msgs book_msgs errors max_orders bbo_changes crossed locked "
        "max_cross_ms crossed_trading executed hidden cross check_fail bbo_hash\n");
    static const std::string_view kKinds[] = {"map", "tick", "tick-rh", "tick-dm",
                                              "tick-aos", "tick-soa", "tick-sv", "btree"};
    if (std::find(std::begin(kKinds), std::end(kKinds), kind) == std::end(kKinds)) {
        std::fprintf(stderr, "unknown book %s\n", argv[2]);
        return 2;
    }
    auto run_one = [&](std::uint16_t loc) {
        Result r;
        if (kind == "map")
            r = replay<book::MapBook>(dir, loc, check_every);
        else if (kind == "tick")
            r = replay<book::TickBook<book::LinearMap>>(dir, loc, check_every);
        else if (kind == "tick-rh")
            r = replay<book::TickBook<book::RobinHoodMap>>(dir, loc, check_every);
        else if (kind == "tick-dm")
            r = replay<book::TickBook<book::DirectMap<>>>(dir, loc, check_every);
        else if (kind == "tick-aos")
            r = replay<book::TickBook<book::LinearMap, book::Aos>>(dir, loc, check_every);
        else if (kind == "tick-soa")
            r = replay<book::TickBook<book::LinearMap, book::Soa>>(dir, loc, check_every);
        else if (kind == "tick-sv")
            r = replay<book::SortedVecBook<>>(dir, loc, check_every);
        else if (kind == "btree")
            r = replay<book::BTreeBook<>>(dir, loc, check_every);
        else
            r = replay<book::BTreeBook<>>(dir, loc, check_every);
        if (r.max_cross_ns > kPersistentCrossNs) ++r.check_failures;
        return r;
    };

    // Symbols are independent, so they replay on a pool; rows still print in locate order.
    const char* te = std::getenv("THREADS");
    const unsigned threads =
        te ? static_cast<unsigned>(std::strtoul(te, nullptr, 10)) : std::thread::hardware_concurrency();
    std::vector<Result> results(locates.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < locates.size();) results[i] = run_one(locates[i]);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < std::max(threads, 1u); ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();

    int rc = 0;
    for (std::size_t i = 0; i < locates.size(); ++i) {
        const std::uint16_t loc = locates[i];
        const Result& r = results[i];
        if (r.stats.errors || r.check_failures) rc = 1;
        std::printf("%u %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                    " %" PRIu64 " %.3f %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                    " %016" PRIx64 "\n",
                    loc, r.symbol.empty() ? "-" : r.symbol.c_str(), r.msgs, r.stats.book_msgs,
                    r.stats.errors, r.max_orders, r.bbo_changes, r.crossed, r.locked,
                    r.max_cross_ns / 1e6, r.crossed_trading, r.stats.executed, r.stats.hidden,
                    r.stats.cross, r.check_failures, r.hash.h);
    }
    return rc;
}
