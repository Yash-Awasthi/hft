#include "data/store.hpp"

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

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

std::vector<IndexEntry> read_index(const std::filesystem::path& dir, std::uint16_t locate) {
    std::ifstream idx(path_for(dir, locate, ".idx"), std::ios::binary | std::ios::ate);
    if (!idx) throw std::runtime_error("no index for locate " + std::to_string(locate));
    std::vector<IndexEntry> out(static_cast<std::size_t>(idx.tellg()) / sizeof(IndexEntry));
    idx.seekg(0);
    idx.read(reinterpret_cast<char*>(out.data()),
             static_cast<std::streamsize>(out.size() * sizeof(IndexEntry)));
    if (!idx) throw std::runtime_error("cannot read index for locate " + std::to_string(locate));
    return out;
}

// Reads and decompresses one chunk into `raw`; `comp` is scratch.
void read_chunk(const std::filesystem::path& zst_path, const IndexEntry& e,
                std::vector<std::uint8_t>& comp, std::vector<std::uint8_t>& raw) {
    comp.resize(e.comp_size);
    // Opened per chunk: a merge holds thousands of readers, more than the fd limit.
    std::ifstream zst(zst_path, std::ios::binary);
    zst.seekg(static_cast<std::streamoff>(e.offset));
    zst.read(reinterpret_cast<char*>(comp.data()), e.comp_size);
    if (!zst) throw std::runtime_error("short read on chunk");
    raw.resize(e.raw_size);
    const std::size_t n = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    if (ZSTD_isError(n) || n != e.raw_size) throw std::runtime_error("corrupt chunk");
}

// Parses the record at `pos`; false if it overruns the chunk.
bool parse_record(const std::vector<std::uint8_t>& raw, std::size_t pos, Record& out) {
    if (pos + kRecordHeader > raw.size()) return false;
    std::uint16_t len;
    std::memcpy(&out.seq, &raw[pos], 8);
    std::memcpy(&len, &raw[pos + 8], 2);
    if (pos + kRecordHeader + len > raw.size()) return false;
    out.data = &raw[pos + kRecordHeader];
    out.len = len;
    return true;
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

SymbolReader::SymbolReader(const std::filesystem::path& dir, std::uint16_t locate)
    : idx_(read_index(dir, locate)), zst_path_(path_for(dir, locate, ".zst")) {}

void SymbolReader::load(std::size_t chunk) {
    std::vector<std::uint8_t> comp;
    read_chunk(zst_path_, idx_[chunk], comp, raw_);
    pos_ = 0;
}

bool SymbolReader::next(Record& out) {
    while (pos_ >= raw_.size()) {
        if (chunk_ >= idx_.size()) return false;
        load(chunk_++);
    }
    if (!parse_record(raw_, pos_, out)) throw std::runtime_error("corrupt record");
    pos_ += kRecordHeader + out.len;
    return true;
}

MergedReader::MergedReader(const std::filesystem::path& dir, std::vector<std::uint16_t> locates,
                           std::size_t ahead, unsigned threads)
    : ahead_(ahead ? ahead : 1), slots_(kWindow), bits_(kWindow / 64, 0), word_(kWindow / 64) {
    for (std::uint16_t loc : locates) {
        auto idx = read_index(dir, loc);
        if (idx.empty()) continue;
        const auto sym = static_cast<std::uint32_t>(syms_.size());
        for (const IndexEntry& e : idx) order_.push_back({e.first_seq, sym, e});
        heap_.push_back({idx[0].first_seq, sym});
        syms_.push_back(Sym{loc, path_for(dir, loc, ".zst"), idx, {}, 0, 0});
    }
    std::sort(order_.begin(), order_.end(),
              [](const Pending& a, const Pending& b) { return a.first_seq < b.first_seq; });
    std::make_heap(heap_.begin(), heap_.end(), std::greater<>());
    for (unsigned t = 0; t < std::max(threads, 1u); ++t) threads_.emplace_back([this] { run(); });
}

MergedReader::~MergedReader() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    space_.notify_all();
    for (auto& t : threads_) t.join();
}

void MergedReader::run() {
    try {
        std::vector<std::uint8_t> comp;
        for (;;) {
            std::size_t k;
            std::vector<std::uint8_t> raw;
            {
                std::unique_lock lock(mu_);
                space_.wait(lock, [this] {
                    return claimed_ == order_.size() || claimed_ - popped_ < ahead_ || stop_;
                });
                if (stop_ || claimed_ == order_.size()) return;
                k = claimed_++;
                ready_.push_back({order_[k].sym, {}});
                if (!spare_.empty()) {
                    raw = std::move(spare_.back());
                    spare_.pop_back();
                }
            }
            const Pending& p = order_[k];
            read_chunk(syms_[p.sym].zst_path, p.entry, comp, raw);
            std::lock_guard lock(mu_);
            Ready& r = ready_[k - popped_];
            r.raw = std::move(raw);
            r.done = true;
            ++done_;
            if (k == popped_ || done_ == order_.size()) data_.notify_all();
        }
    } catch (...) {
        std::lock_guard lock(mu_);
        if (!error_) error_ = std::current_exception();
        data_.notify_all();
    }
}

void MergedReader::take_chunk(std::uint32_t sym) {
    std::unique_lock lock(mu_);
    data_.wait(lock, [this] { return (!ready_.empty() && ready_.front().done) || error_; });
    if (ready_.empty() || !ready_.front().done) std::rethrow_exception(error_);
    if (ready_.front().sym != sym) throw std::logic_error("chunks out of order");
    syms_[sym].bufs.push_back(std::move(ready_.front().raw));
    ready_.pop_front();
    ++popped_;
    space_.notify_all();
    ++syms_[sym].chunk;
}

void MergedReader::wait_read_ahead() {
    if (ahead_ < order_.size()) throw std::logic_error("read-ahead smaller than the chunk count");
    std::unique_lock lock(mu_);
    data_.wait(lock, [this] { return done_ == order_.size() || error_; });
    if (error_) std::rethrow_exception(error_);
}

// Scatters every record with a sequence number in [base, base + kWindow) into the window.
// base is the smallest next sequence number, and every chunk starting below the window's
// end is taken first, so each symbol's records for the window are already in memory.
bool MergedReader::fill() {
    if (!retired_.empty()) {
        std::lock_guard lock(mu_);
        for (auto& b : retired_) spare_.push_back(std::move(b));
        retired_.clear();
    }
    if (heap_.empty()) return false;
    base_ = heap_.front().seq;
    const std::uint64_t end = base_ + kWindow;
    while (taken_ < order_.size() && order_[taken_].first_seq < end)
        take_chunk(order_[taken_++].sym);

    while (!heap_.empty() && heap_.front().seq < end) {
        const std::uint32_t sym = heap_.front().sym;
        Sym& s = syms_[sym];
        std::uint64_t key = UINT64_MAX;
        while (!s.bufs.empty()) {
            if (s.pos >= s.bufs.front().size()) {
                retired_.push_back(std::move(s.bufs.front()));
                s.bufs.pop_front();
                s.pos = 0;
                continue;
            }
            Record rec{};
            if (!parse_record(s.bufs.front(), s.pos, rec))
                throw std::runtime_error("corrupt record");
            if (rec.seq >= end) {
                key = rec.seq;
                break;
            }
            const std::uint64_t i = rec.seq - base_;
            if (rec.seq < base_ || bits_[i / 64] >> (i % 64) & 1)
                throw std::runtime_error("sequence numbers not increasing");
            slots_[i] = {rec.data, rec.len, sym};
            bits_[i / 64] |= std::uint64_t{1} << (i % 64);
            s.pos += kRecordHeader + rec.len;
        }
        if (s.bufs.empty() && s.chunk < s.idx.size()) key = s.idx[s.chunk].first_seq;
        if (key == UINT64_MAX) {
            std::pop_heap(heap_.begin(), heap_.end(), std::greater<>());
            heap_.pop_back();
        } else {
            sift_down(Head{key, sym});
        }
    }
    word_ = 0;
    cur_ = std::exchange(bits_[0], 0);
    return true;
}

bool MergedReader::next(Record& out, std::uint16_t& locate) {
    while (cur_ == 0) {
        if (++word_ < bits_.size()) {
            cur_ = std::exchange(bits_[word_], 0);
        } else if (!fill()) {
            return false;
        }
    }
    const std::size_t i = word_ * 64 + static_cast<std::size_t>(std::countr_zero(cur_));
    cur_ &= cur_ - 1;
    const Slot& sl = slots_[i];
    out = {base_ + i, sl.data, sl.len};
    locate = syms_[sl.sym].locate;
    return true;
}

void MergedReader::sift_down(Head h) {
    const std::size_t n = heap_.size();
    std::size_t i = 0;
    for (;;) {
        std::size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && heap_[c + 1].seq < heap_[c].seq) ++c;
        if (h.seq <= heap_[c].seq) break;
        heap_[i] = heap_[c];
        i = c;
    }
    heap_[i] = h;
}

}  // namespace hft::data
