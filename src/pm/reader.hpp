#pragma once

// Reads a pm_record directory: calls `fn(recv_ns, event)` for every JSON event in receive
// order within each connection (batched messages arrive one event at a time). Connections
// carry disjoint tokens, so per-token logic needs no ordering between them.

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "net/json.hpp"

namespace hft::pm {

inline std::string read_record_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (p.extension() != ".zst") return s;
    const unsigned long long n = ZSTD_getFrameContentSize(s.data(), s.size());
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN)
        throw std::runtime_error("bad frame: " + p.string());
    std::string out(n, '\0');
    const std::size_t got = ZSTD_decompress(out.data(), out.size(), s.data(), s.size());
    if (ZSTD_isError(got)) throw std::runtime_error("decompress failed: " + p.string());
    return out;
}

struct ReadStats {
    std::size_t files = 0;
    std::uint64_t messages = 0, parse_errors = 0;
    double max_gap_s = 0;  // longest silence on any connection
};

template <class Fn>
ReadStats read_records(const std::filesystem::path& root, Fn&& fn) {
    namespace fs = std::filesystem;
    ReadStats st;
    std::vector<fs::path> conns;
    for (const auto& e : fs::directory_iterator(root))
        if (e.is_directory()) conns.push_back(e.path());
    std::sort(conns.begin(), conns.end());
    for (const fs::path& c : conns) {
        std::vector<fs::path> hours;
        for (const auto& e : fs::directory_iterator(c)) hours.push_back(e.path());
        std::sort(hours.begin(), hours.end());
        std::int64_t last = 0;
        for (const fs::path& h : hours) {
            const std::string data = read_record_file(h);
            ++st.files;
            for (std::size_t i = 0; i < data.size();) {
                std::size_t nl = data.find('\n', i);
                if (nl == std::string::npos) nl = data.size();
                const std::string_view line = std::string_view(data).substr(i, nl - i);
                i = nl + 1;
                const std::size_t sp = line.find(' ');
                if (sp == std::string_view::npos) {
                    if (!line.empty()) ++st.parse_errors;
                    continue;
                }
                const std::int64_t ns = std::strtoll(std::string(line.substr(0, sp)).c_str(), nullptr, 10);
                if (last) st.max_gap_s = std::max(st.max_gap_s, static_cast<double>(ns - last) * 1e-9);
                last = ns;
                ++st.messages;
                try {
                    const net::Json j = net::parse_json(line.substr(sp + 1));
                    if (j.type == net::Json::Type::Arr)
                        for (const net::Json& e : j.a) fn(ns, e);
                    else
                        fn(ns, j);
                } catch (const std::runtime_error&) {
                    ++st.parse_errors;
                }
            }
        }
    }
    return st;
}

}  // namespace hft::pm
