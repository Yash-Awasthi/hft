// Merges every symbol in a store back into one BinaryFILE stream on stdout, in feed order.
// Usage: store_cat <store-dir> | sha256sum

#include <cstdio>
#include <filesystem>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "core/endian.hpp"
#include "data/store.hpp"

namespace {

struct Head {
    std::uint64_t seq;
    std::size_t reader;
    bool operator>(const Head& o) const { return seq > o.seq; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: store_cat <store-dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];

    std::vector<std::unique_ptr<hft::data::SymbolReader>> readers;
    std::vector<hft::data::Record> cur;
    std::priority_queue<Head, std::vector<Head>, std::greater<>> heap;

    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".idx") continue;
        const auto locate = static_cast<std::uint16_t>(std::stoul(e.path().stem().string()));
        readers.push_back(std::make_unique<hft::data::SymbolReader>(dir, locate));
        cur.emplace_back();
        if (readers.back()->next(cur.back())) heap.push({cur.back().seq, readers.size() - 1});
    }

    while (!heap.empty()) {
        const Head h = heap.top();
        heap.pop();
        const hft::data::Record& r = cur[h.reader];
        const std::uint16_t be = hft::be16_to_host(r.len);
        std::fwrite(&be, 2, 1, stdout);
        std::fwrite(r.data, 1, r.len, stdout);
        if (readers[h.reader]->next(cur[h.reader])) heap.push({cur[h.reader].seq, h.reader});
    }
    return 0;
}
