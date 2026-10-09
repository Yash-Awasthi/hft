#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "net/json.hpp"
#include "net/tls.hpp"
#include "net/ws_frames.hpp"

using namespace hft::net;

namespace {

std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : d) s += k[b >> 4], s += k[b & 15];
    return s;
}

}  // namespace

TEST(Net, Sha1KnownAnswers) {
    EXPECT_EQ(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Net, AcceptKeyMatchesRfcExample) {
    EXPECT_EQ(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(Net, Base64Padding) {
    auto enc = [](std::string s) { return base64(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); };
    EXPECT_EQ(enc(""), "");
    EXPECT_EQ(enc("f"), "Zg==");
    EXPECT_EQ(enc("fo"), "Zm8=");
    EXPECT_EQ(enc("foo"), "Zm9v");
}

TEST(Net, FramesRoundTripAtEveryLengthClass) {
    const std::array<std::uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};
    for (const std::size_t n : {0u, 5u, 125u, 126u, 300u, 65535u, 65536u, 70000u}) {
        std::string payload(n, 'x');
        for (std::size_t i = 0; i < n; ++i) payload[i] = static_cast<char>('a' + i % 26);
        const std::string f = ws_client_frame(kText, payload, mask);
        WsParser p;
        WsMessage m;
        // One byte at a time: the parser must wait for the rest of a frame.
        for (std::size_t i = 0; i + 1 < f.size(); ++i) {
            p.push(&f[i], 1);
            ASSERT_FALSE(p.next(m)) << "n=" << n << " i=" << i;
        }
        p.push(&f.back(), 1);
        ASSERT_TRUE(p.next(m));
        EXPECT_EQ(m.op, kText);
        EXPECT_EQ(m.data, payload);
        EXPECT_FALSE(p.next(m));
    }
}

TEST(Net, FragmentsJoinAndControlFramesInterleave) {
    // "Hel" (text, not final) + ping + "lo" (continuation, final); server frames are unmasked.
    std::string wire;
    wire += std::string("\x01\x03Hel", 5);
    wire += std::string("\x89\x01!", 3);
    wire += std::string("\x80\x02lo", 4);
    WsParser p;
    p.push(wire.data(), wire.size());
    WsMessage m;
    ASSERT_TRUE(p.next(m));
    EXPECT_EQ(m.op, kPing);
    EXPECT_EQ(m.data, "!");
    ASSERT_TRUE(p.next(m));
    EXPECT_EQ(m.op, kText);
    EXPECT_EQ(m.data, "Hello");
}

TEST(Net, ParserRejectsBadFrames) {
    WsMessage m;
    {
        WsParser p;  // reserved bit set
        const std::string f("\xC1\x00", 2);
        p.push(f.data(), f.size());
        EXPECT_THROW(p.next(m), std::runtime_error);
    }
    {
        WsParser p;  // continuation with nothing to continue
        const std::string f("\x80\x00", 2);
        p.push(f.data(), f.size());
        EXPECT_THROW(p.next(m), std::runtime_error);
    }
    {
        WsParser p;  // fragmented control frame
        const std::string f("\x09\x00", 2);
        p.push(f.data(), f.size());
        EXPECT_THROW(p.next(m), std::runtime_error);
    }
}

TEST(Json, ParsesNestedValuesAndEscapes) {
    const Json j = parse_json(
        R"({"a":[1,2.5,-3e2],"s":"x\"y\n\u00e9\ud83d\ude00","t":true,"f":false,"n":null,"o":{"k":"v"}})");
    ASSERT_EQ(j.type, Json::Type::Obj);
    EXPECT_EQ(j.find("a")->a.size(), 3u);
    EXPECT_DOUBLE_EQ(j.find("a")->a[2].n, -300.0);
    EXPECT_EQ(j.str("s"), "x\"y\n\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_TRUE(j.flag("t"));
    EXPECT_FALSE(j.flag("f"));
    EXPECT_EQ(j.find("n")->type, Json::Type::Null);
    EXPECT_EQ(j.find("o")->str("k"), "v");
    EXPECT_EQ(j.str("missing"), "");
}

TEST(Json, RejectsMalformedInput) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\"}", "\"abc", "[1 2]", "tru", "{\"a\":1} x", "\"\\ud800\"",
                            "\"\\q\""})
        EXPECT_THROW(parse_json(bad), std::runtime_error) << bad;
    EXPECT_THROW(parse_json(std::string(100, '[')), std::runtime_error);
}

TEST(Net, WriteToClosedPeerFailsInsteadOfKillingProcess) {
    TlsConn c;
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ::close(sv[1]);
    EXPECT_EQ(::write(sv[0], "x", 1), -1);
    EXPECT_EQ(errno, EPIPE);
    ::close(sv[0]);
}

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <thread>

#include "net/http_server.hpp"

namespace {

std::string http_fetch(std::uint16_t port, const std::string& request) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return "connect failed";
    ::send(s, request.data(), request.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[1024];
    for (ssize_t n; (n = ::recv(s, buf, sizeof buf, 0)) > 0;) out.append(buf, static_cast<std::size_t>(n));
    ::close(s);
    return out;
}

}  // namespace

TEST(Http, RequestPath) {
    EXPECT_EQ(request_path("GET /metrics HTTP/1.1\r\n"), "/metrics");
    EXPECT_EQ(request_path("GET /m?x=1 HTTP/1.1\r\n"), "/m");
    EXPECT_EQ(request_path("POST /m HTTP/1.1\r\n"), "");
    EXPECT_EQ(request_path("GET /m"), "");
}

TEST(Http, ServesRoutesOnLoopback) {
    HttpServer srv;
    ASSERT_TRUE(srv.listen(0));
    std::atomic<bool> stop{false};
    std::thread t([&] {
        while (!stop)
            srv.poll_once(50, [](std::string_view p) {
                if (p == "/metrics") return HttpResponse{200, "text/plain", "up 1\n"};
                return HttpResponse{404, "text/plain", "no\n"};
            });
    });
    const std::string ok = http_fetch(srv.port(), "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string missing = http_fetch(srv.port(), "GET /nope HTTP/1.1\r\n\r\n");
    const std::string post = http_fetch(srv.port(), "POST /metrics HTTP/1.1\r\n\r\n");
    stop = true;
    t.join();
    EXPECT_TRUE(ok.starts_with("HTTP/1.1 200 OK\r\n")) << ok;
    EXPECT_TRUE(ok.ends_with("\r\n\r\nup 1\n")) << ok;
    EXPECT_NE(ok.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_TRUE(missing.starts_with("HTTP/1.1 404 ")) << missing;
    EXPECT_TRUE(post.starts_with("HTTP/1.1 405 ")) << post;
}
