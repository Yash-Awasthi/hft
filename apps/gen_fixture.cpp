// Writes a synthetic ITCH day as a zstd-compressed BinaryFILE stream, for test fixtures.
// Usage: gen_fixture <seed> <symbols> <messages> <out.zst>

#include <zstd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "sources/random_flow.hpp"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: gen_fixture <seed> <symbols> <messages> <out.zst>\n");
        return 2;
    }
    std::vector<std::uint8_t> raw;
    hft::sources::RandomFlow flow(std::strtoull(argv[1], nullptr, 10),
                                  static_cast<std::uint16_t>(std::atoi(argv[2])));
    flow.day(std::strtoull(argv[3], nullptr, 10), raw);
    std::vector<std::uint8_t> comp(ZSTD_compressBound(raw.size()));
    const std::size_t n = ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), 19);
    if (ZSTD_isError(n)) return 1;
    std::FILE* f = std::fopen(argv[4], "wb");
    if (!f || std::fwrite(comp.data(), 1, n, f) != n) return 1;
    std::fclose(f);
    std::printf("%zu raw bytes, %zu compressed\n", raw.size(), n);
    return 0;
}
