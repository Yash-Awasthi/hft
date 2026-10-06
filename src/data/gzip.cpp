#include "data/gzip.hpp"

#include <zlib-ng.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/sha256.hpp"

namespace hft::data {

namespace {

constexpr int kGzipWindowBits = 15 + 16;
constexpr std::size_t kInputChunk = 1 << 20;

}  // namespace

struct GzipReader::Impl {
    std::FILE* file = nullptr;
    zng_stream strm{};
    std::vector<std::uint8_t> in = std::vector<std::uint8_t>(kInputChunk);
    Sha256 sha;
    bool done = false;

    // Refills the input buffer; returns false at the end of the file.
    bool refill() {
        const std::size_t got = std::fread(in.data(), 1, in.size(), file);
        if (got == 0) return false;
        sha.update(in.data(), got);
        strm.next_in = in.data();
        strm.avail_in = static_cast<std::uint32_t>(got);
        return true;
    }
};

GzipReader::GzipReader(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    impl_->file = std::fopen(path.c_str(), "rb");
    if (!impl_->file) throw std::runtime_error("cannot open " + path.string());
    if (zng_inflateInit2(&impl_->strm, kGzipWindowBits) != Z_OK) {
        std::fclose(impl_->file);
        throw std::runtime_error("zlib-ng init failed");
    }
}

GzipReader::~GzipReader() {
    zng_inflateEnd(&impl_->strm);
    std::fclose(impl_->file);
}

std::size_t GzipReader::read(std::uint8_t* dst, std::size_t n) {
    Impl& s = *impl_;
    s.strm.next_out = dst;
    s.strm.avail_out = static_cast<std::uint32_t>(n);

    while (s.strm.avail_out > 0 && !s.done) {
        if (s.strm.avail_in == 0 && !s.refill()) {
            throw std::runtime_error("gzip stream truncated");
        }
        const int rc = zng_inflate(&s.strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            // Another member may follow; the end of the file ends the stream.
            if (s.strm.avail_in == 0 && !s.refill()) {
                s.done = true;
            } else {
                zng_inflateReset(&s.strm);
            }
        } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            throw std::runtime_error(std::string("gzip error: ") +
                                     (s.strm.msg ? s.strm.msg : "unknown"));
        }
    }
    return n - s.strm.avail_out;
}

std::string GzipReader::sha256_hex() { return impl_->sha.hex(); }

}  // namespace hft::data
