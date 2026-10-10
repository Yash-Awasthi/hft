// The tape and the tree reader must accept the same inputs, and the SIMD and scalar stage 1
// must find the same structure; the message decoder must give the same events either way.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "net/json.hpp"
#include "net/tape.hpp"
#include "pm/msg.hpp"

namespace {

bool same(const hft::net::JsonTape& a, const hft::net::JsonTape& b, std::uint32_t i) {
    if (a.type(i) != b.type(i) || a.raw(i) != b.raw(i) || a.end(i) != b.end(i)) return false;
    for (std::uint32_t c = a.first(i); c != a.end(i); c = a.next(c))
        if (!same(a, b, c)) return false;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view s(reinterpret_cast<const char*>(data), size);
    bool tree = true;
    try {
        hft::net::parse_json(s);
    } catch (const std::runtime_error&) {
        tree = false;
    }
    hft::net::JsonTape simd, scalar;
    scalar.use_simd(false);
    const bool a = simd.parse(s), b = scalar.parse(s);
    if (a != b || a != tree) std::abort();
    if (a && !same(simd, scalar, simd.root())) std::abort();

    // The message decoder's single pass must give the tape's events, or defer to it.
    hft::pm::Decoder fast, tape;
    std::vector<hft::pm::Event> x, y;
    std::vector<std::vector<hft::pm::Level>> xl, yl;
    std::vector<std::vector<hft::pm::Change>> xc, yc;
    auto keep = [](auto& ev, auto& lv, auto& ch) {
        return [&](const hft::pm::Event& e) {
            ev.push_back(e);
            lv.emplace_back(e.bids.begin(), e.bids.end()), lv.back().insert(lv.back().end(), e.asks.begin(), e.asks.end());
            ch.emplace_back(e.changes.begin(), e.changes.end());
        };
    };
    if (fast.decode(s, keep(x, xl, xc)) != tape.decode_tape(s, keep(y, yl, yc)) || x.size() != y.size()) std::abort();
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto &p = x[i], &q = y[i];
        if (p.kind != q.kind || p.type != q.type || p.asset != q.asset || p.tick != q.tick || p.has_changes != q.has_changes ||
            p.px != q.px || p.price != q.price || std::memcmp(&p.size, &q.size, sizeof p.size) || p.buy != q.buy ||
            p.exch_ms != q.exch_ms || p.conn != q.conn || p.bids.size() != q.bids.size() || xl[i].size() != yl[i].size() ||
            xc[i].size() != yc[i].size())
            std::abort();
        for (std::size_t k = 0; k < xl[i].size(); ++k)
            if (xl[i][k].px != yl[i][k].px || std::memcmp(&xl[i][k].size, &yl[i][k].size, sizeof(double))) std::abort();
        for (std::size_t k = 0; k < xc[i].size(); ++k) {
            const auto &c = xc[i][k], &d = yc[i][k];
            if (c.asset != d.asset || c.px != d.px || std::memcmp(&c.size, &d.size, sizeof(double)) || c.buy != d.buy ||
                c.best_bid != d.best_bid || c.best_ask != d.best_ask)
                std::abort();
        }
    }
    return 0;
}
