#include "data/gzip.hpp"

#include <gtest/gtest.h>
#include <zlib-ng.h>

#include <fstream>
#include <string>
#include <vector>

#include "core/sha256.hpp"
#include "data/prefetch.hpp"
#include "temp_dir.hpp"

using namespace hft::data;

namespace {

std::string sha_of(const std::string& s) {
    hft::Sha256 h;
    h.update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    return h.hex();
}

std::string gzip(const std::string& s) {
    zng_stream z{};
    EXPECT_EQ(zng_deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY), Z_OK);
    std::string out(zng_deflateBound(&z, s.size()) + 64, '\0');
    z.next_in = reinterpret_cast<const std::uint8_t*>(s.data());
    z.avail_in = static_cast<std::uint32_t>(s.size());
    z.next_out = reinterpret_cast<std::uint8_t*>(out.data());
    z.avail_out = static_cast<std::uint32_t>(out.size());
    EXPECT_EQ(zng_deflate(&z, Z_FINISH), Z_STREAM_END);
    out.resize(z.total_out);
    zng_deflateEnd(&z);
    return out;
}

std::string write(const TempDir& dir, const std::string& bytes) {
    const std::string p = (dir.path / "f.gz").string();
    std::ofstream(p, std::ios::binary) << bytes;
    return p;
}

template <class R>
std::string drain(R& r, std::size_t step) {
    std::string out, buf(step, '\0');
    while (const std::size_t n = r.read(reinterpret_cast<std::uint8_t*>(buf.data()), step)) {
        out.append(buf, 0, n);
    }
    return out;
}

std::string payload(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; s.size() < n; ++i)
        s += "line " + std::to_string(i * 7919 % 1000) + "\n";
    s.resize(n);
    return s;
}

}  // namespace

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(sha_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha_of(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ChunkedUpdateMatchesOneShot) {
    const std::string s = payload(10007);
    hft::Sha256 h;
    for (std::size_t i = 0; i < s.size(); i += 37) {
        h.update(reinterpret_cast<const std::uint8_t*>(s.data()) + i,
                 std::min<std::size_t>(37, s.size() - i));
    }
    EXPECT_EQ(h.hex(), sha_of(s));
}

TEST(Gzip, RoundtripAndHash) {
    TempDir dir("gzip");
    const std::string data = payload(3'000'000);
    const std::string gz = gzip(data);
    GzipReader r(write(dir, gz));
    EXPECT_EQ(drain(r, 4096) == data, true);
    EXPECT_EQ(r.sha256_hex(), sha_of(gz));
}

TEST(Gzip, ConcatenatedMembersReadAsOneStream) {
    TempDir dir("gzip");
    GzipReader r(write(dir, gzip("hello ") + gzip("world")));
    EXPECT_EQ(drain(r, 3), "hello world");
}

TEST(Gzip, TruncatedInputThrows) {
    TempDir dir("gzip");
    std::string gz = gzip(payload(100000));
    gz.resize(gz.size() / 2);
    GzipReader r(write(dir, gz));
    EXPECT_THROW(drain(r, 4096), std::runtime_error);
}

TEST(Gzip, BadChecksumThrows) {
    TempDir dir("gzip");
    std::string gz = gzip(payload(100000));
    gz[gz.size() - 8] ^= 0x01;  // CRC-32 in the trailer
    GzipReader r(write(dir, gz));
    EXPECT_THROW(drain(r, 4096), std::runtime_error);
}

TEST(Prefetch, MatchesDirectReadWithSmallBlocks) {
    TempDir dir("gzip");
    const std::string data = payload(500'000);
    GzipReader r(write(dir, gzip(data)));
    Prefetch<GzipReader> p(r, 1000, 2);
    EXPECT_EQ(drain(p, 777) == data, true);
}

TEST(Prefetch, PropagatesSourceError) {
    TempDir dir("gzip");
    std::string gz = gzip(payload(100000));
    gz.resize(gz.size() / 2);
    GzipReader r(write(dir, gz));
    Prefetch<GzipReader> p(r, 1000, 2);
    EXPECT_THROW(drain(p, 4096), std::runtime_error);
}

TEST(Prefetch, DestructorStopsWorkerWithUnreadData) {
    TempDir dir("gzip");
    GzipReader r(write(dir, gzip(payload(1'000'000))));
    Prefetch<GzipReader> p(r, 1000, 2);
    std::string buf(10, '\0');
    EXPECT_EQ(p.read(reinterpret_cast<std::uint8_t*>(buf.data()), 10), 10u);
}
