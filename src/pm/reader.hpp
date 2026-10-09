#pragma once

// Reads a pm_record directory: calls `fn(recv_ns, event)` for every event in receive order
// within each connection (batched messages arrive one event at a time). Connections carry
// disjoint tokens, so per-token logic needs no ordering between them. A second thread reads
// and decompresses ahead of the parser, in fixed blocks, so memory stays flat.

#include <zstd.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/spsc.hpp"
#include "pm/msg.hpp"

namespace hft::pm {

struct ReadStats {
    std::size_t files = 0;
    std::uint64_t messages = 0, parse_errors = 0;
    double max_gap_s = 0;  // longest silence on any connection
};

// Feeds the decompressed bytes of one record file (.jsonl or .jsonl.zst) to `chunk`.
template <class Chunk>
void stream_record_file(const std::filesystem::path& p, Chunk&& chunk) {
    struct Close {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<std::FILE, Close> f(std::fopen(p.c_str(), "rb"));
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::vector<char> in(1 << 20), out(ZSTD_DStreamOutSize() * 4);
    if (p.extension() != ".zst") {
        while (const std::size_t n = std::fread(in.data(), 1, in.size(), f.get())) chunk(std::string_view(in.data(), n));
        return;
    }
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> z(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    std::size_t last = 0;
    while (const std::size_t n = std::fread(in.data(), 1, in.size(), f.get())) {
        ZSTD_inBuffer ib{in.data(), n, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob{out.data(), out.size(), 0};
            last = ZSTD_decompressStream(z.get(), &ob, &ib);
            if (ZSTD_isError(last)) throw std::runtime_error("decompress failed: " + p.string());
            chunk(std::string_view(out.data(), ob.pos));
        }
    }
    if (last != 0) throw std::runtime_error("truncated frame: " + p.string());
}

namespace detail {

// Decompressed text handed from the reading thread to the parsing thread in blocks that
// end on a line boundary, so the parser never sees half a line.
struct Item {
    enum Kind : std::uint8_t { Data, File, Conn, Done, Error } kind;
    std::uint32_t block = 0, len = 0;
};

class Pipeline {
   public:
    static constexpr std::size_t kBlocks = 8, kBlockSize = 1 << 20;

    explicit Pipeline(const std::filesystem::path& root) : mem_(new char[kBlocks * kBlockSize]) {
        for (std::uint32_t b = 0; b < kBlocks; ++b) free_.push(b);
        reader_ = std::thread([this, root] { run(root); });
    }
    ~Pipeline() {
        // Drain so the reader can finish after an early exit of the parser.
        Item it;
        while (!done_)
            if (full_.try_pop(it)) {
                if (it.kind == Item::Data) free_.push(it.block);
                done_ = it.kind == Item::Done || it.kind == Item::Error;
            }
        reader_.join();
    }

    Item next() {
        const Item it = full_.pop();
        if (it.kind == Item::Done) done_ = true;
        if (it.kind == Item::Error) {
            done_ = true;
            std::rethrow_exception(error_);
        }
        return it;
    }
    std::string_view text(const Item& it) const { return {mem_.get() + it.block * kBlockSize, it.len}; }
    void release(const Item& it) { free_.push(it.block); }

   private:
    char* block(std::uint32_t b) { return mem_.get() + b * kBlockSize; }

    void run(const std::filesystem::path& root) {
        namespace fs = std::filesystem;
        try {
            std::vector<fs::path> conns;
            for (const auto& e : fs::directory_iterator(root))
                if (e.is_directory()) conns.push_back(e.path());
            std::sort(conns.begin(), conns.end());
            std::uint32_t cur = free_.pop();
            std::size_t fill = 0;
            // Hands over the block up to its last newline (all of it at the end of a file).
            auto flush = [&](bool all) {
                std::size_t cut = fill;
                if (!all) {
                    while (cut > 0 && block(cur)[cut - 1] != '\n') --cut;
                    if (cut == 0) throw std::runtime_error("record line longer than a block");
                }
                const std::uint32_t next = free_.pop();
                std::memcpy(block(next), block(cur) + cut, fill - cut);
                full_.push({Item::Data, cur, static_cast<std::uint32_t>(cut)});
                cur = next, fill -= cut;
            };
            for (const fs::path& c : conns) {
                full_.push({Item::Conn});
                std::vector<fs::path> hours;
                for (const auto& e : fs::directory_iterator(c)) hours.push_back(e.path());
                std::sort(hours.begin(), hours.end());
                for (const fs::path& h : hours) {
                    full_.push({Item::File});
                    stream_record_file(h, [&](std::string_view d) {
                        while (!d.empty()) {
                            const std::size_t n = std::min(d.size(), kBlockSize - fill);
                            std::memcpy(block(cur) + fill, d.data(), n);
                            fill += n, d.remove_prefix(n);
                            if (fill == kBlockSize) flush(false);
                        }
                    });
                    if (fill) flush(true);
                }
            }
            full_.push({Item::Done});
        } catch (...) {
            error_ = std::current_exception();
            full_.push({Item::Error});
        }
    }

    std::unique_ptr<char[]> mem_;
    SpscRing<Item, 16> full_;
    SpscRing<std::uint32_t, 16> free_;  // filled by the parser, drained by the reader
    std::exception_ptr error_;
    std::thread reader_;
    bool done_ = false;
};

}  // namespace detail

template <class Fn>
ReadStats read_records(const std::filesystem::path& root, Fn&& fn) {
    ReadStats st;
    Decoder dec;
    std::int64_t last = 0;
    auto line = [&](std::string_view l) {
        const std::size_t sp = l.find(' ');
        if (sp == std::string_view::npos) {
            if (!l.empty()) ++st.parse_errors;
            return;
        }
        std::int64_t ns = 0;
        std::from_chars(l.data(), l.data() + sp, ns);
        if (last) st.max_gap_s = std::max(st.max_gap_s, static_cast<double>(ns - last) * 1e-9);
        last = ns;
        ++st.messages;
        if (!dec.decode(l.substr(sp + 1), [&](const Event& e) { fn(ns, e); })) ++st.parse_errors;
    };
    detail::Pipeline pipe(root);
    for (;;) {
        const detail::Item it = pipe.next();
        if (it.kind == detail::Item::Done) break;
        if (it.kind == detail::Item::Conn) last = 0;
        if (it.kind == detail::Item::File) ++st.files;
        if (it.kind != detail::Item::Data) continue;
        const std::string_view d = pipe.text(it);
        for (std::size_t i = 0; i < d.size();) {
            std::size_t nl = d.find('\n', i);
            if (nl == std::string_view::npos) nl = d.size();
            line(d.substr(i, nl - i));
            i = nl + 1;
        }
        pipe.release(it);
    }
    return st;
}

}  // namespace hft::pm
