// Splits a BinaryFILE ITCH stream into the per-symbol zstd store.
// Usage: ingest <empty-output-dir> [day.gz]
// With a .gz argument the file is decompressed in-process, the gzip CRC is verified and the
// SHA-256 of the compressed file is recorded in <output-dir>/source.sha256. Without it the
// raw stream is read from stdin.

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "data/gzip.hpp"
#include "data/prefetch.hpp"
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

int run(const std::filesystem::path& out, const char* gz_path) {
    using namespace hft::itch;
    std::filesystem::create_directories(out);
    if (!std::filesystem::is_empty(out)) {
        std::fprintf(stderr, "output dir not empty\n");
        return 2;
    }

    std::unique_ptr<hft::data::GzipReader> gz;
    std::unique_ptr<hft::data::Prefetch<hft::data::GzipReader>> prefetch;
    std::function<std::size_t(std::uint8_t*, std::size_t)> read_chunk;
    if (gz_path) {
        gz = std::make_unique<hft::data::GzipReader>(gz_path);
        prefetch = std::make_unique<hft::data::Prefetch<hft::data::GzipReader>>(*gz);
        read_chunk = [&](std::uint8_t* p, std::size_t n) { return prefetch->read(p, n); };
    } else {
        read_chunk = [](std::uint8_t* p, std::size_t n) { return std::fread(p, 1, n, stdin); };
    }

    hft::data::StoreWriter writer(out, kBudget, kChunkMsgs);
    SymbolMap symbols;
    std::vector<std::uint8_t> buf(kChunk + 2 + 65535);
    std::size_t have = 0;
    unsigned long long seq = 0, bad = 0;

    for (;;) {
        const std::size_t got = read_chunk(buf.data() + have, kChunk);
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

    if (gz) {
        const std::string sha = gz->sha256_hex();
        std::ofstream(out / "source.sha256")
            << sha << "  " << std::filesystem::path(gz_path).filename().string() << '\n';
        std::printf("source_sha256 %s\n", sha.c_str());
    }
    std::printf(
        "frames %llu\nbad %llu\nsymbols %zu\ntrailing_bytes %zu\nraw_bytes %llu\n"
        "compressed_bytes %llu\n",
        seq, bad, symbols.names.size(), have, static_cast<unsigned long long>(writer.raw_bytes()),
        static_cast<unsigned long long>(writer.compressed_bytes()));
    return (bad || have) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: ingest <output-dir> [day.gz]\n");
        return 2;
    }
    try {
        return run(argv[1], argc == 3 ? argv[2] : nullptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ingest failed: %s\n", e.what());
        return 1;
    }
}
