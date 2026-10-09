#pragma once

// Client TLS socket (OpenSSL for the cipher layer only), an HTTP/1.1 GET and a WebSocket
// client on top of it. Certificates and host names are verified.

#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/ws_frames.hpp"

namespace hft::net {

class TlsConn {
   public:
    // OpenSSL writes with write(2); a peer reset would otherwise kill the process with SIGPIPE.
    TlsConn() { ::signal(SIGPIPE, SIG_IGN); }
    TlsConn(const TlsConn&) = delete;
    TlsConn& operator=(const TlsConn&) = delete;
    ~TlsConn() { close(); }

    void connect(const std::string& host, const std::string& port, int timeout_s = 15) {
        close();
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0)
            throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(rc));
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);
        for (const addrinfo* a = res; a; a = a->ai_next) {
            fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd_ < 0) continue;
            timeval tv{timeout_s, 0};
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
            ::close(fd_);
            fd_ = -1;
        }
        if (fd_ < 0) throw std::runtime_error("connect " + host + ":" + port + " failed");
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("SSL_CTX_new");
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) throw std::runtime_error("tls handshake with " + host + ": " + last_error());
    }

    // Reads up to n bytes; 0 means the peer closed.
    std::size_t read(char* buf, std::size_t n) {
        const int r = SSL_read(ssl_, buf, static_cast<int>(n));
        if (r > 0) return static_cast<std::size_t>(r);
        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        throw std::runtime_error("tls read: " + last_error());
    }

    void write_all(std::string_view s) {
        while (!s.empty()) {
            const int w = SSL_write(ssl_, s.data(), static_cast<int>(s.size()));
            if (w <= 0) throw std::runtime_error("tls write: " + last_error());
            s.remove_prefix(static_cast<std::size_t>(w));
        }
    }

    // True when a read will not block: decrypted bytes pending, or data on the socket.
    bool readable(int timeout_ms) {
        if (SSL_pending(ssl_) > 0) return true;
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0 && errno != EINTR) throw std::runtime_error("poll failed");
        return r > 0;
    }

    void close() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

   private:
    static std::string last_error() {
        char buf[256];
        const unsigned long e = ERR_get_error();
        if (!e) return "connection error";
        ERR_error_string_n(e, buf, sizeof buf);
        return buf;
    }
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

namespace detail {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Value of a header in a raw header block, empty if absent.
inline std::string header_value(const std::string& head, const std::string& name) {
    const std::string h = lower(head), key = "\r\n" + lower(name) + ":";
    const std::size_t p = h.find(key);
    if (p == std::string::npos) return {};
    std::size_t b = p + key.size(), e = h.find("\r\n", b);
    if (e == std::string::npos) e = h.size();
    while (b < e && head[b] == ' ') ++b;
    return head.substr(b, e - b);
}

inline std::string decode_chunked(const std::string& raw) {
    std::string out;
    std::size_t i = 0;
    for (;;) {
        const std::size_t nl = raw.find("\r\n", i);
        if (nl == std::string::npos) throw std::runtime_error("http: bad chunk header");
        const std::size_t len = std::stoul(raw.substr(i, nl - i), nullptr, 16);
        i = nl + 2;
        if (len == 0) return out;
        if (i + len > raw.size()) throw std::runtime_error("http: truncated chunk");
        out.append(raw, i, len);
        i += len + 2;
    }
}

}  // namespace detail

// GET https://host/path; returns the body of a 200 response.
inline std::string http_get(const std::string& host, const std::string& path) {
    TlsConn c;
    c.connect(host, "443");
    c.write_all("GET " + path + " HTTP/1.1\r\nHost: " + host +
                "\r\nUser-Agent: hft-recorder\r\nAccept: application/json\r\n"
                "Accept-Encoding: identity\r\nConnection: close\r\n\r\n");
    std::string raw;
    char buf[16384];
    while (const std::size_t n = c.read(buf, sizeof buf)) raw.append(buf, n);
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) throw std::runtime_error("http: no header block");
    const std::string head = raw.substr(0, split + 2);
    if (head.compare(0, 9, "HTTP/1.1 ") != 0 || head.compare(9, 3, "200") != 0)
        throw std::runtime_error("http: " + head.substr(0, head.find("\r\n")));
    std::string body = raw.substr(split + 4);
    if (detail::lower(detail::header_value(head, "Transfer-Encoding")).find("chunked") != std::string::npos)
        body = detail::decode_chunked(body);
    return body;
}

class WsClient {
   public:
    enum class Status { Message, Timeout, Closed };

    void connect(const std::string& host, const std::string& path) {
        parser_ = WsParser();
        conn_.connect(host, "443");
        std::array<std::uint8_t, 16> nonce{};
        if (::getrandom(nonce.data(), nonce.size(), 0) != static_cast<ssize_t>(nonce.size()))
            throw std::runtime_error("getrandom failed");
        const std::string key = base64(nonce.data(), nonce.size());
        conn_.write_all("GET " + path + " HTTP/1.1\r\nHost: " + host +
                        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                        "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: hft-recorder\r\n\r\n");
        std::string head;
        char c;
        while (head.size() < 8192 && (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0)) {
            if (conn_.read(&c, 1) == 0) throw std::runtime_error("ws: closed during handshake");
            head += c;
        }
        if (head.compare(9, 3, "101") != 0) throw std::runtime_error("ws: " + head.substr(0, head.find("\r\n")));
        if (detail::header_value(head, "Sec-WebSocket-Accept") != ws_accept_key(key))
            throw std::runtime_error("ws: bad Sec-WebSocket-Accept");
    }

    void send_text(std::string_view s) { send(kText, s); }
    void send(int op, std::string_view s) {
        std::array<std::uint8_t, 4> mask{};
        if (::getrandom(mask.data(), mask.size(), 0) != static_cast<ssize_t>(mask.size()))
            throw std::runtime_error("getrandom failed");
        conn_.write_all(ws_client_frame(op, s, mask));
    }

    // Waits up to timeout_ms for a data message; answers pings itself.
    Status read(std::string& text, int timeout_ms) {
        WsMessage m;
        for (;;) {
            while (parser_.next(m)) {
                if (m.op == kPing) {
                    send(kPong, m.data);
                } else if (m.op == kClose) {
                    return Status::Closed;
                } else if (m.op == kText || m.op == kBinary) {
                    text = std::move(m.data);
                    return Status::Message;
                }
            }
            if (!conn_.readable(timeout_ms)) return Status::Timeout;
            char buf[65536];
            const std::size_t n = conn_.read(buf, sizeof buf);
            if (n == 0) return Status::Closed;
            parser_.push(buf, n);
        }
    }

    void close() { conn_.close(); }

   private:
    TlsConn conn_;
    WsParser parser_;
};

}  // namespace hft::net
