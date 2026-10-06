// Splits a BinaryFILE ITCH stream on stdin into the per-symbol zstd store.
// Usage: zcat day.gz | ingest <empty-output-dir>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "data/store.hpp"
#include "feed/itch.hpp"

namespace {

constexpr std::size_t kChunk = 1 << 20;
constexpr std::size_t kBudget = std::size_t{1} << 30;
constexpr std::uint32_t kChunkMsgs = 1 << 20;

struct SymbolMap {
    std::map<std::uint16_t, std::string> names;
    template <class T>
    void operator()(const T&) {}
    void operator()(const hft::itch::StockDirectory& m) {
        std::string s(m.stock, 8);
        s.erase(s.find_last_not_of(' ') + 1);
        names[m.h.locate] = s;
    }
};

}  // namespace

int main(int argc, char** argv) {
    using namespace hft::itch;
    if (argc != 2) {
        std::fprintf(stderr, "usage: ingest <output-dir>\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    if (!std::filesystem::is_empty(out)) {
        std::fprintf(stderr, "output dir not empty\n");
        return 2;
    }

    hft::data::StoreWriter writer(out, kBudget, kChunkMsgs);
    SymbolMap symbols;
    std::vector<std::uint8_t> buf(kChunk + 2 + 65535);
    std::size_t have = 0;
    unsigned long long seq = 0, bad = 0;

    for (;;) {
        const std::size_t got = std::fread(buf.data() + have, 1, kChunk, stdin);
        have += got;
        std::size_t off = 0;
        Frame f{};
        while (const std::size_t used = next_frame(buf.data() + off, have - off, f)) {
            off += used;
            if (dispatch(f.data, f.size, symbols) != Status::Ok) {
                ++bad;
            } else {
                writer.append(hft::load_be16(f.data + 1), seq, hft::load_be48(f.data + 5), f.data,
                              f.size);
            }
            ++seq;
        }
        std::memmove(buf.data(), buf.data() + off, have - off);
        have -= off;
        if (got == 0) break;
    }
    writer.finish();

    std::ofstream tsv(out / "symbols.tsv");
    for (const auto& [locate, name] : symbols.names) tsv << locate << '\t' << name << '\n';

    std::printf(
        "frames %llu\nbad %llu\nsymbols %zu\ntrailing_bytes %zu\nraw_bytes %llu\n"
        "compressed_bytes %llu\n",
        seq, bad, symbols.names.size(), have, static_cast<unsigned long long>(writer.raw_bytes()),
        static_cast<unsigned long long>(writer.compressed_bytes()));
    return bad ? 1 : 0;
}
