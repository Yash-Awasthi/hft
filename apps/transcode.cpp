// Rewrites a store with another chunk codec, keeping chunk boundaries so checkpoints and
// sequence ranges stay valid. Symbols run on a pool (THREADS, default all hardware threads).
// Usage: transcode <src-store> <dst-store> [--codec lz4|zstd]    (default lz4)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "data/store.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    using hft::data::Codec;
    Codec codec = Codec::Lz4;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            const char* c = argv[++i];
            if (std::strcmp(c, "zstd") == 0)
                codec = Codec::Zstd;
            else if (std::strcmp(c, "lz4") != 0)
                args.clear(), args.resize(3);  // rejected below
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: transcode <src-store> <dst-store> [--codec lz4|zstd]\n");
        return 2;
    }
    const fs::path src = args[0], dst = args[1];
    fs::create_directories(dst);
    if (!fs::is_empty(dst)) {
        std::fprintf(stderr, "output dir not empty\n");
        return 2;
    }

    std::vector<std::uint16_t> locates;
    for (const auto& e : fs::directory_iterator(src)) {
        const std::string ext = e.path().extension().string();
        if (ext == ".idx")
            locates.push_back(static_cast<std::uint16_t>(std::stoul(e.path().stem().string())));
        else if (ext == ".tsv" || ext == ".sha256" || ext == ".ckp" || ext == ".cki")
            fs::copy_file(e.path(), dst / e.path().filename());
    }
    std::sort(locates.begin(), locates.end());

    const char* te = std::getenv("THREADS");
    const unsigned threads =
        te ? static_cast<unsigned>(std::strtoul(te, nullptr, 10)) : std::thread::hardware_concurrency();
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> bytes{0};
    auto worker = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < locates.size();)
            bytes += hft::data::transcode_symbol(src, dst, locates[i], codec);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < std::max(threads, 1u); ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    std::printf("symbols %zu\ncompressed_bytes %llu\n", locates.size(),
                static_cast<unsigned long long>(bytes.load()));
    return 0;
}
