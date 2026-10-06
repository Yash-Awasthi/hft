#pragma once

// Chunked zstd store, one file pair per stock-locate code:
//   <locate>.zst  concatenated zstd frames, each holding ~chunk_msgs records
//   <locate>.idx  one IndexEntry per frame
// A record is {u64 seq, u16 len, len message bytes}, host (little) endian. The global
// sequence number lets symbols be merged back into feed order.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace hft::data {

static_assert(std::endian::native == std::endian::little, "store format is little-endian");

struct IndexEntry {
    std::uint64_t offset;  // of the frame in the .zst file
    std::uint32_t comp_size;
    std::uint32_t raw_size;
    std::uint32_t n_msgs;
    std::uint32_t reserved;
    std::uint64_t first_ts;
    std::uint64_t first_seq;
};
static_assert(sizeof(IndexEntry) == 40);

class StoreWriter {
   public:
    // Buffers up to `budget_bytes` across all symbols; the largest buffers are flushed
    // first when it is exceeded. The directory must already exist and be empty.
    StoreWriter(std::filesystem::path dir, std::size_t budget_bytes, std::uint32_t chunk_msgs);
    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;
    ~StoreWriter();

    void append(std::uint16_t locate, std::uint64_t seq, std::uint64_t ts, const std::uint8_t* msg,
                std::size_t len);
    void finish();

    std::uint64_t raw_bytes() const { return raw_total_; }
    std::uint64_t compressed_bytes() const { return comp_total_; }

   private:
    struct Buf {
        std::vector<std::uint8_t> raw;
        std::uint32_t n = 0;
        std::uint64_t first_ts = 0;
        std::uint64_t first_seq = 0;
    };

    void flush(std::uint16_t locate);

    std::filesystem::path dir_;
    std::size_t budget_;
    std::uint32_t chunk_msgs_;
    std::vector<Buf> bufs_;
    std::vector<std::uint64_t> offset_;
    std::vector<std::uint8_t> scratch_;
    std::size_t buffered_ = 0;
    std::uint64_t raw_total_ = 0;
    std::uint64_t comp_total_ = 0;
};

struct Record {
    std::uint64_t seq;
    const std::uint8_t* data;  // valid until the next call to next()
    std::uint16_t len;
};

class SymbolReader {
   public:
    SymbolReader(const std::filesystem::path& dir, std::uint16_t locate);
    bool next(Record& out);

   private:
    void load(std::size_t chunk);

    std::vector<IndexEntry> idx_;
    std::filesystem::path zst_path_;
    std::vector<std::uint8_t> raw_;
    std::size_t chunk_ = 0;
    std::size_t pos_ = 0;
};

}  // namespace hft::data
