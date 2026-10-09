#pragma once

// Loopback HTTP/1.1 server for metrics and a dashboard: GET only, one request per
// connection, served from the thread that calls poll_once. Not meant for the open network.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace hft::net {

struct HttpResponse {
    int status = 200;
    std::string type = "text/plain; charset=utf-8";
    std::string body;
};

// "GET /a?b HTTP/1.1\r\n..." -> "/a"; empty if the request is not a GET.
inline std::string_view request_path(std::string_view req) {
    if (!req.starts_with("GET ")) return {};
    req.remove_prefix(4);
    const std::size_t end = req.find_first_of(" ?\r\n");
    return end == std::string_view::npos ? std::string_view() : req.substr(0, end);
}

class HttpServer {
   public:
    HttpServer() = default;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() {
        if (fd_ >= 0) ::close(fd_);
    }

    // Binds 127.0.0.1:port (0 picks a free port). False with errno set on failure.
    bool listen(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) return false;
        socklen_t n = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &n);
        port_ = ntohs(a.sin_port);
        return true;
    }
    std::uint16_t port() const { return port_; }

    // Serves at most one connection, waiting up to timeout_ms for it; `handler(path)`
    // returns the response.
    template <class Handler>
    void poll_once(int timeout_ms, Handler&& handler) {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return;
        const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) return;
        timeval tv{1, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        std::string req;
        char buf[2048];
        while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
        }
        const std::string_view path = request_path(req);
        HttpResponse r;
        if (path.empty()) r = {405, "text/plain; charset=utf-8", "GET only\n"};
        else r = handler(path);
        const char* reason = r.status == 200 ? "OK" : r.status == 404 ? "Not Found" : "Error";
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\nContent-Type: " + r.type +
                          "\r\nContent-Length: " + std::to_string(r.body.size()) +
                          "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + r.body;
        for (std::size_t off = 0; off < out.size();) {
            const ssize_t n = ::send(c, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
        ::close(c);
    }

   private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace hft::net
