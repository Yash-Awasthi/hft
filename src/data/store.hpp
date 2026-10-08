#pragma once

// Chunked zstd store, one file pair per stock-locate code:
//   <locate>.zst  concatenated zstd frames, each holding ~chunk_msgs records
//   <locate>.idx  one IndexEntry per frame
// A record is {u64 seq, u16 len, len message bytes}, host (little) endian. The global
// sequence number lets symbols be merged back into feed order.

#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
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
    // Positions the reader at the first record of chunk `chunk`.
    void seek(std::size_t chunk) {
        chunk_ = chunk;
        raw_.clear();
        pos_ = 0;
    }
    const std::vector<IndexEntry>& index() const { return idx_; }

   private:
    void load(std::size_t chunk);

    std::vector<IndexEntry> idx_;
    std::filesystem::path zst_path_;
    std::vector<std::uint8_t> raw_;
    std::size_t chunk_ = 0;
    std::size_t pos_ = 0;
};

// Merges the records of several symbols back into feed order. Read-ahead threads
// decompress chunks in global first-sequence order, which is exactly the order the merge
// takes them in, so the hand-off is a bounded FIFO of at most `ahead` chunks; with several
// threads a chunk can finish early but is handed over only in that order. Records are
// merged a window of kWindow sequence numbers at a time: each symbol scatters its records
// into one slot per sequence number, then the window is emitted by scanning a bitmap.
class MergedReader {
   public:
    MergedReader(const std::filesystem::path& dir, std::vector<std::uint16_t> locates,
                 std::size_t ahead = 16, unsigned threads = 1);
    ~MergedReader();
    MergedReader(const MergedReader&) = delete;
    MergedReader& operator=(const MergedReader&) = delete;

    // `out.data` stays valid until the next call.
    bool next(Record& out, std::uint16_t& locate);
    // Blocks until every chunk is decompressed, so the merge and decode can be timed alone.
    // Needs `ahead` of at least the chunk count.
    void wait_read_ahead();

    static constexpr std::size_t kWindow = 4096;

   private:
    struct Head {
        std::uint64_t seq;
        std::uint32_t sym;
        bool operator>(const Head& o) const { return seq > o.seq; }
    };
    struct Sym {
        std::uint16_t locate;
        std::filesystem::path zst_path;
        std::vector<IndexEntry> idx;
        std::deque<std::vector<std::uint8_t>> bufs;  // taken, front is being read
        std::size_t pos;
        std::size_t chunk;  // chunks taken so far
    };
    struct Pending {
        std::uint64_t first_seq;
        std::uint32_t sym;
        IndexEntry entry;
    };
    struct Ready {
        std::uint32_t sym;
        std::vector<std::uint8_t> raw;
        bool done = false;
    };
    struct Slot {
        const std::uint8_t* data;
        std::uint16_t len;
        std::uint16_t locate;
        std::uint32_t sym;
    };

    void run();
    void take_chunk(std::uint32_t sym);
    bool fill();
    void sift_down(Head h);

    std::size_t ahead_;
    std::vector<Sym> syms_;
    std::vector<Pending> order_;
    std::size_t taken_ = 0;
    std::vector<Head> heap_;  // symbols by next sequence number

    std::uint64_t base_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint64_t> bits_;
    std::size_t word_;
    std::uint64_t cur_ = 0;
    std::vector<std::vector<std::uint8_t>> retired_;  // read in this window, recycled after it

    std::mutex mu_;
    std::condition_variable data_, space_;
    std::deque<Ready> ready_;  // chunks popped_, popped_ + 1, ... claimed by a thread
    std::size_t popped_ = 0;
    std::size_t claimed_ = 0;
    std::size_t done_ = 0;
    std::vector<std::vector<std::uint8_t>> spare_;
    std::exception_ptr error_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace hft::data
