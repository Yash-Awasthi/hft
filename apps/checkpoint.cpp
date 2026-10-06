// Writes or verifies per-symbol book checkpoints beside the store chunks: the book state at
// the first chunk boundary after every N messages, plus the end-of-day state, so a book at
// any time is one checkpoint load and at most N messages plus one chunk of replay away.
// Symbols with fewer than N messages get none.
//   <locate>.ckp  concatenated zstd frames, one per checkpoint
//   <locate>.cki  one CheckpointEntry per checkpoint
// Usage: checkpoint <store-dir> [--verify] [locate ...]   (no locate: every symbol)
// Env: CHECKPOINT_EVERY=N (default 1000000).
// Verify loads each checkpoint, replays up to the next and requires the state to equal it
// byte for byte.

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "book/itch_apply.hpp"
#include "book/tick_book.hpp"
#include "data/store.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft;
using Book = book::TickBook<book::LinearMap>;

struct CheckpointEntry {
    std::uint64_t offset;
    std::uint32_t comp_size;
    std::uint32_t raw_size;
    std::uint32_t chunk;  // state before this chunk; == number of chunks for end of day
    std::uint32_t reserved;
    std::uint64_t seq;  // first sequence number of that chunk, or last applied + 1 at end
};
static_assert(sizeof(CheckpointEntry) == 32);

std::filesystem::path path_for(const std::filesystem::path& dir, std::uint16_t loc,
                               const char* ext) {
    char name[16];
    std::snprintf(name, sizeof name, "%05u%s", loc, ext);
    return dir / name;
}

// Replays `n` records (all when n == 0) and returns the number applied.
std::uint64_t play(data::SymbolReader& rd, book::ItchApply<Book>& ap, std::uint64_t n) {
    data::Record rec{};
    std::uint64_t done = 0;
    while ((n == 0 || done < n) && rd.next(rec)) {
        ap.seq = rec.seq;
        itch::dispatch(rec.data, rec.len, ap);
        ++done;
    }
    return done;
}

std::uint64_t write(const std::filesystem::path& dir, std::uint16_t loc, std::uint64_t every) {
    data::SymbolReader rd(dir, loc);
    const auto& idx = rd.index();
    std::uint64_t msgs = 0;
    for (const auto& e : idx) msgs += e.n_msgs;
    if (msgs < every) return 0;
    Book b;
    book::ItchApply<Book> ap{b};
    std::ofstream ckp(path_for(dir, loc, ".ckp"), std::ios::binary | std::ios::trunc);
    std::ofstream cki(path_for(dir, loc, ".cki"), std::ios::binary | std::ios::trunc);
    std::vector<std::uint8_t> raw, comp;
    std::uint64_t offset = 0, since = 0, written = 0;
    for (std::size_t k = 0; k <= idx.size(); ++k) {
        if (since >= every || k == idx.size()) {
            since = 0;
            ++written;
            b.save(raw);
            comp.resize(ZSTD_compressBound(raw.size()));
            const std::size_t n =
                ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), 3);
            if (ZSTD_isError(n)) throw std::runtime_error("zstd compress failed");
            const std::uint64_t seq = k < idx.size() ? idx[k].first_seq : ap.seq + 1;
            const CheckpointEntry e{offset,
                                    static_cast<std::uint32_t>(n),
                                    static_cast<std::uint32_t>(raw.size()),
                                    static_cast<std::uint32_t>(k),
                                    0,
                                    seq};
            ckp.write(reinterpret_cast<const char*>(comp.data()), static_cast<std::streamsize>(n));
            cki.write(reinterpret_cast<const char*>(&e), sizeof e);
            offset += n;
        }
        if (k < idx.size()) since += play(rd, ap, idx[k].n_msgs);
    }
    if (!ckp || !cki) throw std::runtime_error("checkpoint write failed");
    return written;
}

std::vector<std::uint8_t> read_state(std::ifstream& ckp, const CheckpointEntry& e) {
    std::vector<std::uint8_t> comp(e.comp_size), raw(e.raw_size);
    ckp.seekg(static_cast<std::streamoff>(e.offset));
    ckp.read(reinterpret_cast<char*>(comp.data()), e.comp_size);
    if (!ckp) throw std::runtime_error("short checkpoint read");
    const std::size_t n = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    if (ZSTD_isError(n) || n != raw.size()) throw std::runtime_error("corrupt checkpoint");
    return raw;
}

// Returns the number of checkpoints that failed to reproduce their successor.
std::uint64_t verify(const std::filesystem::path& dir, std::uint16_t loc) {
    std::ifstream cki(path_for(dir, loc, ".cki"), std::ios::binary);
    if (!cki) return 0;
    std::vector<CheckpointEntry> es;
    CheckpointEntry e{};
    while (cki.read(reinterpret_cast<char*>(&e), sizeof e)) es.push_back(e);
    std::ifstream ckp(path_for(dir, loc, ".ckp"), std::ios::binary);
    data::SymbolReader rd(dir, loc);
    std::uint64_t bad = 0;
    std::vector<std::uint8_t> got;
    for (std::size_t i = 0; i + 1 < es.size(); ++i) {
        Book b;
        const std::vector<std::uint8_t> s = read_state(ckp, es[i]);
        if (!b.load(s.data(), s.size()) || !b.check()) {
            ++bad;
            continue;
        }
        book::ItchApply<Book> ap{b};
        rd.seek(es[i].chunk);
        for (std::uint32_t k = es[i].chunk; k < es[i + 1].chunk; ++k)
            play(rd, ap, rd.index()[k].n_msgs);
        b.save(got);
        bad += got != read_state(ckp, es[i + 1]);
    }
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: checkpoint <store-dir> [--verify] [locate ...]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    int i = 2;
    const bool check = argc > 2 && std::strcmp(argv[2], "--verify") == 0;
    if (check) ++i;
    const char* ev = std::getenv("CHECKPOINT_EVERY");
    const std::uint64_t every = ev ? std::strtoull(ev, nullptr, 10) : 1'000'000;
    std::vector<std::uint16_t> locates;
    for (; i < argc; ++i) locates.push_back(static_cast<std::uint16_t>(std::atoi(argv[i])));
    if (locates.empty())
        for (const auto& en : std::filesystem::directory_iterator(dir))
            if (en.path().extension() == ".idx")
                locates.push_back(
                    static_cast<std::uint16_t>(std::stoul(en.path().stem().string())));
    std::sort(locates.begin(), locates.end());

    std::uint64_t total = 0, bad = 0;
    for (std::uint16_t loc : locates) {
        if (check) {
            const std::uint64_t b = verify(dir, loc);
            if (b)
                std::printf("locate %u: %llu checkpoints differ\n", loc,
                            static_cast<unsigned long long>(b));
            bad += b;
        } else {
            total += write(dir, loc, every);
        }
    }
    if (check)
        std::printf("%s\n", bad ? "FAILED" : "ok");
    else
        std::printf("%llu checkpoints\n", static_cast<unsigned long long>(total));
    return bad ? 1 : 0;
}
