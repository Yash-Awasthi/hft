// Prints one canonical line per ITCH message of a BinaryFILE stream on stdin, for
// field-level comparison with other parsers (archive/research/compare_itchfeed.py).
// Usage: itch_dump <skip> <count> <slice-out>   writes the dumped messages to slice-out too.
// Line: type|locate|tracking|timestamp|fields...; types this decoder leaves undecoded
// (Y L V W K J h N O) print the header only.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/endian.hpp"
#include "feed/itch.hpp"

namespace {

using namespace hft::itch;

struct Dump {
    std::string s;
    void head(const Header& h) {
        s.clear();
        s += h.type;
        num(h.locate);
        num(h.tracking);
        num(h.timestamp);
    }
    void num(std::uint64_t v) { s += "|" + std::to_string(v); }
    void ch(char c) {
        s += '|';
        s += c;
    }
    template <std::size_t N>
    void str(const char (&c)[N]) {
        s += '|';
        s.append(c, N);
    }

    void operator()(const Header& h) { head(h); }
    void operator()(const SystemEvent& m) {
        head(m.h);
        ch(m.event);
    }
    void operator()(const StockDirectory& m) {
        head(m.h);
        str(m.stock);
        ch(m.market_category);
        ch(m.financial_status);
        num(m.round_lot_size);
        ch(m.round_lots_only);
        ch(m.issue_classification);
        str(m.issue_subtype);
        ch(m.authenticity);
        ch(m.short_sale_threshold);
        ch(m.ipo_flag);
        ch(m.luld_tier);
        ch(m.etp_flag);
        num(m.etp_leverage);
        ch(m.inverse);
    }
    void operator()(const TradingAction& m) {
        head(m.h);
        str(m.stock);
        ch(m.state);
        ch(m.reserved);
        str(m.reason);
    }
    void operator()(const AddOrder& m) {
        head(m.h);
        num(m.ref);
        ch(m.side);
        num(m.shares);
        str(m.stock);
        num(m.price);
        if (m.h.type == 'F') str(m.mpid);
    }
    void operator()(const OrderExecuted& m) {
        head(m.h);
        num(m.ref);
        num(m.shares);
        num(m.match);
    }
    void operator()(const OrderExecutedPrice& m) {
        head(m.h);
        num(m.ref);
        num(m.shares);
        num(m.match);
        ch(m.printable);
        num(m.price);
    }
    void operator()(const OrderCancel& m) {
        head(m.h);
        num(m.ref);
        num(m.cancelled);
    }
    void operator()(const OrderDelete& m) {
        head(m.h);
        num(m.ref);
    }
    void operator()(const OrderReplace& m) {
        head(m.h);
        num(m.orig_ref);
        num(m.new_ref);
        num(m.shares);
        num(m.price);
    }
    void operator()(const Trade& m) {
        head(m.h);
        num(m.ref);
        ch(m.side);
        num(m.shares);
        str(m.stock);
        num(m.price);
        num(m.match);
    }
    void operator()(const CrossTrade& m) {
        head(m.h);
        num(m.shares);
        str(m.stock);
        num(m.price);
        num(m.match);
        ch(m.cross_type);
    }
    void operator()(const BrokenTrade& m) {
        head(m.h);
        num(m.match);
    }
    void operator()(const Imbalance& m) {
        head(m.h);
        num(m.paired);
        num(m.imbalance);
        ch(m.direction);
        str(m.stock);
        num(m.far_price);
        num(m.near_price);
        num(m.ref_price);
        ch(m.cross_type);
        ch(m.variation);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: itch_dump <skip> <count> <slice-out> < stream\n");
        return 2;
    }
    const std::uint64_t skip = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t count = std::strtoull(argv[2], nullptr, 10);
    std::FILE* slice = std::fopen(argv[3], "wb");
    if (!slice) return 2;
    std::vector<std::uint8_t> msg(65536);
    Dump d;
    std::uint64_t n = 0, bad = 0;
    std::uint8_t len_be[2];
    while (n < skip + count && std::fread(len_be, 1, 2, stdin) == 2) {
        const std::size_t len = hft::load_be16(len_be);
        if (std::fread(msg.data(), 1, len, stdin) != len) break;
        if (n++ < skip) continue;
        std::fwrite(len_be, 1, 2, slice);
        std::fwrite(msg.data(), 1, len, slice);
        if (dispatch(msg.data(), len, d) != Status::Ok) {
            ++bad;
            continue;
        }
        std::puts(d.s.c_str());
    }
    std::fclose(slice);
    return bad ? 1 : 0;
}
