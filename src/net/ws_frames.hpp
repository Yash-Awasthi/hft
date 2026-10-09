#pragma once

// WebSocket (RFC 6455) framing without any I/O: SHA-1 and base64 for the opening handshake,
// a client frame writer, and an incremental parser for server frames. The socket layer
// feeds bytes in and takes whole messages out, so everything here is testable on its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hft::net {

inline std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int s = 56; s >= 0; s -= 8) msg += static_cast<char>((bits >> s) & 0xFF);
    auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&msg[off + 4 * static_cast<std::size_t>(i)]);
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

inline std::string base64(const std::uint8_t* p, std::size_t n) {
    static constexpr char kA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (std::uint32_t{p[i]} << 16) | (i + 1 < n ? std::uint32_t{p[i + 1]} << 8 : 0) |
                                (i + 2 < n ? p[i + 2] : 0);
        out += kA[(v >> 18) & 63];
        out += kA[(v >> 12) & 63];
        out += i + 1 < n ? kA[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kA[v & 63] : '=';
    }
    return out;
}

// Value the server must return in Sec-WebSocket-Accept for a given Sec-WebSocket-Key.
inline std::string ws_accept_key(std::string_view key) {
    const auto d = sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(d.data(), d.size());
}

enum WsOp : int { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

// One masked client frame, FIN set.
inline std::string ws_client_frame(int op, std::string_view payload, const std::array<std::uint8_t, 4>& mask) {
    std::string f;
    f += static_cast<char>(0x80 | op);
    const std::size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(0x80 | n);
    } else if (n < 65536) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int s = 56; s >= 0; s -= 8) f += static_cast<char>((static_cast<std::uint64_t>(n) >> s) & 0xFF);
    }
    for (std::uint8_t m : mask) f += static_cast<char>(m);
    for (std::size_t i = 0; i < n; ++i) f += static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
    return f;
}

struct WsMessage {
    int op = kText;
    std::string data;
};

// Feed bytes with push(); take complete messages with next(). Data frames split over
// several fragments come out as one message; control frames come out as they arrive.
class WsParser {
   public:
    static constexpr std::size_t kMaxMessage = 64u << 20;

    void push(const char* p, std::size_t n) { buf_.append(p, n); }

    bool next(WsMessage& out) {
        for (;;) {
            if (buf_.size() - off_ < 2) return compact(), false;
            const auto* h = reinterpret_cast<const std::uint8_t*>(buf_.data() + off_);
            const bool fin = h[0] & 0x80;
            const int op = h[0] & 0x0F;
            if (h[0] & 0x70) throw std::runtime_error("ws: reserved bits set");
            const bool masked = h[1] & 0x80;
            std::uint64_t len = h[1] & 0x7F;
            std::size_t hdr = 2;
            if (len == 126) {
                if (buf_.size() - off_ < 4) return compact(), false;
                len = (std::uint64_t{h[2]} << 8) | h[3];
                hdr = 4;
            } else if (len == 127) {
                if (buf_.size() - off_ < 10) return compact(), false;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | h[2 + i];
                hdr = 10;
            }
            if (len > kMaxMessage) throw std::runtime_error("ws: frame too large");
            const std::size_t mask_len = masked ? 4 : 0;
            if (buf_.size() - off_ < hdr + mask_len + len) return compact(), false;
            std::string payload(buf_, off_ + hdr + mask_len, static_cast<std::size_t>(len));
            if (masked)
                for (std::size_t i = 0; i < payload.size(); ++i)
                    payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ h[hdr + i % 4]);
            off_ += hdr + mask_len + static_cast<std::size_t>(len);

            if (op >= 8) {  // control frames are never fragmented
                if (!fin || len > 125) throw std::runtime_error("ws: bad control frame");
                out.op = op;
                out.data = std::move(payload);
                return true;
            }
            if (op != kCont) {
                if (in_frag_) throw std::runtime_error("ws: new message inside a fragmented one");
                frag_op_ = op;
                frag_.clear();
            } else if (!in_frag_) {
                throw std::runtime_error("ws: continuation without a message");
            }
            frag_ += payload;
            if (frag_.size() > kMaxMessage) throw std::runtime_error("ws: message too large");
            in_frag_ = !fin;
            if (fin) {
                out.op = frag_op_;
                out.data = std::move(frag_);
                frag_.clear();
                return true;
            }
        }
    }

   private:
    void compact() {
        if (off_ > 0 && off_ * 2 >= buf_.size()) {
            buf_.erase(0, off_);
            off_ = 0;
        }
    }
    std::string buf_, frag_;
    std::size_t off_ = 0;
    int frag_op_ = kText;
    bool in_frag_ = false;
};

}  // namespace hft::net
