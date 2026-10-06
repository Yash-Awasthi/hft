#include "data/store.hpp"

#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hft::data {

namespace {

constexpr std::size_t kLocates = 65536;
constexpr int kLevel = 3;
constexpr std::size_t kRecordHeader = 10;  // u64 seq + u16 len

std::filesystem::path path_for(const std::filesystem::path& dir, std::uint16_t locate,
                               const char* ext) {
    char name[16];
    std::snprintf(name, sizeof name, "%05u%s", locate, ext);
    return dir / name;
}

void append_file(const std::filesystem::path& p, const void* data, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!f) throw std::runtime_error("write failed: " + p.string());
}

}  // namespace

StoreWriter::StoreWriter(std::filesystem::path dir, std::size_t budget_bytes,
                         std::uint32_t chunk_msgs)
    : dir_(std::move(dir)),
      budget_(budget_bytes),
      chunk_msgs_(chunk_msgs),
      bufs_(kLocates),
      offset_(kLocates, 0) {}

StoreWriter::~StoreWriter() {
    try {
        finish();
    } catch (...) {
    }
}

void StoreWriter::append(std::uint16_t locate, std::uint64_t seq, std::uint64_t ts,
                         const std::uint8_t* msg, std::size_t len) {
    Buf& b = bufs_[locate];
    if (b.n == 0) {
        b.first_ts = ts;
        b.first_seq = seq;
    }
    const std::size_t at = b.raw.size();
    b.raw.resize(at + kRecordHeader + len);
    const auto l = static_cast<std::uint16_t>(len);
    std::memcpy(&b.raw[at], &seq, 8);
    std::memcpy(&b.raw[at + 8], &l, 2);
    std::memcpy(&b.raw[at + kRecordHeader], msg, len);
    ++b.n;
    buffered_ += kRecordHeader + len;

    if (b.n >= chunk_msgs_) flush(locate);
    if (buffered_ > budget_) {
        // Flush down to 80% so a full budget does not flush on every append.
        while (buffered_ > budget_ / 5 * 4) {
            std::size_t biggest = 0;
            for (std::size_t i = 1; i < kLocates; ++i) {
                if (bufs_[i].raw.size() > bufs_[biggest].raw.size()) biggest = i;
            }
            flush(static_cast<std::uint16_t>(biggest));
        }
    }
}

void StoreWriter::flush(std::uint16_t locate) {
    Buf& b = bufs_[locate];
    if (b.n == 0) return;

    scratch_.resize(ZSTD_compressBound(b.raw.size()));
    const std::size_t csize =
        ZSTD_compress(scratch_.data(), scratch_.size(), b.raw.data(), b.raw.size(), kLevel);
    if (ZSTD_isError(csize)) throw std::runtime_error(ZSTD_getErrorName(csize));

    const IndexEntry e{offset_[locate],
                       static_cast<std::uint32_t>(csize),
                       static_cast<std::uint32_t>(b.raw.size()),
                       b.n,
                       0,
                       b.first_ts,
                       b.first_seq};
    append_file(path_for(dir_, locate, ".zst"), scratch_.data(), csize);
    append_file(path_for(dir_, locate, ".idx"), &e, sizeof e);

    offset_[locate] += csize;
    raw_total_ += b.raw.size();
    comp_total_ += csize;
    buffered_ -= b.raw.size();
    std::vector<std::uint8_t>().swap(b.raw);
    b.n = 0;
}

void StoreWriter::finish() {
    for (std::size_t i = 0; i < kLocates; ++i) flush(static_cast<std::uint16_t>(i));
}

SymbolReader::SymbolReader(const std::filesystem::path& dir, std::uint16_t locate) {
    std::ifstream idx(path_for(dir, locate, ".idx"), std::ios::binary | std::ios::ate);
    if (!idx) throw std::runtime_error("no index for locate " + std::to_string(locate));
    idx_.resize(static_cast<std::size_t>(idx.tellg()) / sizeof(IndexEntry));
    idx.seekg(0);
    idx.read(reinterpret_cast<char*>(idx_.data()),
             static_cast<std::streamsize>(idx_.size() * sizeof(IndexEntry)));
    zst_path_ = path_for(dir, locate, ".zst");
    if (!idx) throw std::runtime_error("cannot read index for locate " + std::to_string(locate));
}

void SymbolReader::load(std::size_t chunk) {
    const IndexEntry& e = idx_[chunk];
    std::vector<std::uint8_t> comp(e.comp_size);
    // Opened per chunk: a merge holds thousands of readers, more than the fd limit.
    std::ifstream zst(zst_path_, std::ios::binary);
    zst.seekg(static_cast<std::streamoff>(e.offset));
    zst.read(reinterpret_cast<char*>(comp.data()), e.comp_size);
    if (!zst) throw std::runtime_error("short read on chunk");
    raw_.resize(e.raw_size);
    const std::size_t n = ZSTD_decompress(raw_.data(), raw_.size(), comp.data(), comp.size());
    if (ZSTD_isError(n) || n != e.raw_size) throw std::runtime_error("corrupt chunk");
    pos_ = 0;
}

bool SymbolReader::next(Record& out) {
    while (pos_ >= raw_.size()) {
        if (chunk_ >= idx_.size()) return false;
        load(chunk_++);
    }
    if (pos_ + kRecordHeader > raw_.size()) throw std::runtime_error("corrupt record");
    std::uint16_t len;
    std::memcpy(&out.seq, &raw_[pos_], 8);
    std::memcpy(&len, &raw_[pos_ + 8], 2);
    if (pos_ + kRecordHeader + len > raw_.size()) throw std::runtime_error("corrupt record");
    out.data = &raw_[pos_ + kRecordHeader];
    out.len = len;
    pos_ += kRecordHeader + len;
    return true;
}

}  // namespace hft::data
