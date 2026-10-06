// Reads a BinaryFILE ITCH stream on stdin and prints message counts per type.
// Usage: zcat day.gz | itch_count

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "feed/itch.hpp"

namespace {

struct Sink {
    template <class T>
    void operator()(const T&) {}
};

constexpr std::size_t kChunk = 1 << 20;

}  // namespace

int main() {
    using namespace hft::itch;
    std::array<unsigned long long, 256> counts{};
    unsigned long long bad = 0, frames = 0;
    std::vector<std::uint8_t> buf(kChunk + 2 + 65535);
    std::size_t have = 0;
    Sink sink;

    for (;;) {
        const std::size_t got = std::fread(buf.data() + have, 1, kChunk, stdin);
        have += got;
        std::size_t off = 0;
        Frame f{};
        while (const std::size_t used = next_frame(buf.data() + off, have - off, f)) {
            off += used;
            ++frames;
            if (f.size > 0) ++counts[f.data[0]];
            if (dispatch(f.data, f.size, sink) != Status::Ok) ++bad;
        }
        std::memmove(buf.data(), buf.data() + off, have - off);
        have -= off;
        if (got == 0) break;
    }

    for (int t = 0; t < 256; ++t) {
        if (counts[t]) std::printf("%c %llu\n", t, counts[t]);
    }
    std::printf("frames %llu\nbad %llu\ntrailing_bytes %zu\n", frames, bad, have);
    return bad ? 1 : 0;
}
