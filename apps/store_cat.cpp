// Merges every symbol in a store back into one BinaryFILE stream on stdout, in feed order.
// Usage: store_cat <store-dir> | sha256sum

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/endian.hpp"
#include "data/store.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: store_cat <store-dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::vector<std::uint16_t> locates;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".idx")
            locates.push_back(static_cast<std::uint16_t>(std::stoul(e.path().stem().string())));

    hft::data::MergedReader reader(dir, locates, 256);
    hft::data::Record r{};
    std::uint16_t locate = 0;
    static char buf[1 << 20];  // outlives main, when stdout is flushed
    std::setvbuf(stdout, buf, _IOFBF, sizeof buf);
    while (reader.next(r, locate)) {
        const std::uint16_t be = hft::be16_to_host(r.len);
        std::fwrite(&be, 2, 1, stdout);
        std::fwrite(r.data, 1, r.len, stdout);
    }
    return 0;
}
