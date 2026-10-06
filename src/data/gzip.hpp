#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace hft::data {

// Streaming gunzip of a file with zlib-ng. The decoder verifies the gzip CRC-32 and
// length; corrupt or truncated input throws. Concatenated gzip members are read as one
// stream. Also hashes the compressed bytes for the provenance record.
class GzipReader {
   public:
    explicit GzipReader(const std::filesystem::path& path);
    ~GzipReader();
    GzipReader(const GzipReader&) = delete;
    GzipReader& operator=(const GzipReader&) = delete;

    // Returns the number of bytes written to dst; 0 only at the end of the stream.
    std::size_t read(std::uint8_t* dst, std::size_t n);

    // SHA-256 of the compressed file. Valid once read() has returned 0.
    std::string sha256_hex();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hft::data
