#pragma once

// Appends ITCH 5.0 messages to a BinaryFILE stream (two-byte big-endian length prefix), for
// the synthetic sources. The length is written when the message builder goes out of scope.

#include <cstdint>
#include <string>
#include <vector>

namespace hft::sources {

struct BinaryFileWriter {
    struct Msg {
        std::vector<std::uint8_t>& o;
        std::size_t len_at;
        Msg& u8(std::uint8_t v) {
            o.push_back(v);
            return *this;
        }
        Msg& be(std::uint64_t v, int bytes) {
            for (int sh = (bytes - 1) * 8; sh >= 0; sh -= 8)
                o.push_back(static_cast<std::uint8_t>(v >> sh));
            return *this;
        }
        Msg& u16(std::uint16_t v) { return be(v, 2); }
        Msg& u32(std::uint32_t v) { return be(v, 4); }
        Msg& u64(std::uint64_t v) { return be(v, 8); }
        Msg& text(const std::string& s, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i)
                u8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ');
            return *this;
        }
        Msg(std::vector<std::uint8_t>& out, std::size_t at) : o(out), len_at(at) {}
        Msg(const Msg&) = delete;
        ~Msg() {
            const std::size_t n = o.size() - len_at - 2;
            o[len_at] = static_cast<std::uint8_t>(n >> 8);
            o[len_at + 1] = static_cast<std::uint8_t>(n);
        }
    };

    // Header: type, locate, tracking number 0, 6-byte timestamp.
    static Msg msg(std::vector<std::uint8_t>& out, char type, std::uint16_t locate,
                   std::uint64_t ts) {
        const std::size_t at = out.size();
        out.push_back(0);
        out.push_back(0);
        out.push_back(static_cast<std::uint8_t>(type));
        out.push_back(static_cast<std::uint8_t>(locate >> 8));
        out.push_back(static_cast<std::uint8_t>(locate));
        out.push_back(0);
        out.push_back(0);
        for (int sh = 40; sh >= 0; sh -= 8) out.push_back(static_cast<std::uint8_t>(ts >> sh));
        return Msg(out, at);
    }

    static void system(std::vector<std::uint8_t>& out, char event, std::uint64_t ts) {
        msg(out, 'S', 0, ts).u8(static_cast<std::uint8_t>(event));
    }

    // Common stock on Nasdaq Global Select, round lot 100, not an ETP.
    static void directory(std::vector<std::uint8_t>& out, std::uint16_t locate,
                          const std::string& name, std::uint64_t ts) {
        msg(out, 'R', locate, ts)
            .text(name, 8)
            .u8('Q')
            .u8('N')
            .u32(100)
            .u8('N')
            .u8('C')
            .text("Z", 2)
            .u8('P')
            .u8('N')
            .u8('N')
            .u8('1')
            .u8('N')
            .u32(0)
            .u8('N');
    }
};

}  // namespace hft::sources
