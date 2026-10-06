#include "feed/itch.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <variant>
#include <vector>

using namespace hft::itch;

namespace {

struct Writer {
    std::vector<std::uint8_t> b;

    Writer(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        u8(static_cast<std::uint8_t>(type));
        u16(locate);
        u16(tracking);
        for (int s = 40; s >= 0; s -= 8) u8(static_cast<std::uint8_t>(ts >> s));
    }
    Writer& u8(std::uint8_t v) {
        b.push_back(v);
        return *this;
    }
    Writer& u16(std::uint16_t v) { return u8(v >> 8).u8(v); }
    Writer& u32(std::uint32_t v) { return u16(v >> 16).u16(v); }
    Writer& u64(std::uint64_t v) { return u32(v >> 32).u32(v); }
    Writer& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    Writer& str(std::string_view s) {
        for (char c : s) ch(c);
        return *this;
    }
};

using Any = std::variant<std::monostate, SystemEvent, StockDirectory, TradingAction, AddOrder,
                         OrderExecuted, OrderExecutedPrice, OrderCancel, OrderDelete, OrderReplace,
                         Trade, CrossTrade, BrokenTrade, Imbalance, Header>;

struct Recorder {
    Any got;
    template <class T>
    void operator()(const T& m) {
        got = m;
    }
};

Status decode(const Writer& w, Recorder& r) { return dispatch(w.b.data(), w.b.size(), r); }

std::string_view sv(const char* p, std::size_t n) { return {p, n}; }

constexpr std::uint64_t kTs = 34'200'123'456'789;  // 09:30:00.123456789

}  // namespace

TEST(Itch, LengthTableMatchesSpec) {
    EXPECT_EQ(length_of('A'), 36u);
    EXPECT_EQ(length_of('F'), 40u);
    EXPECT_EQ(length_of('R'), 39u);
    EXPECT_EQ(length_of('I'), 50u);
    EXPECT_EQ(length_of('z'), 0u);
}

TEST(Itch, AddOrder) {
    Writer w('A', 7, 3, kTs);
    w.u64(123456789012).ch('B').u32(300).str("AAPL    ").u32(1895500);
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<AddOrder>(r.got);
    EXPECT_EQ(m.h.locate, 7);
    EXPECT_EQ(m.h.tracking, 3);
    EXPECT_EQ(m.h.timestamp, kTs);
    EXPECT_EQ(m.ref, 123456789012u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(sv(m.stock, 8), "AAPL    ");
    EXPECT_EQ(m.price, 1895500u);
    EXPECT_EQ(sv(m.mpid, 4), std::string_view("\0\0\0\0", 4));
}

TEST(Itch, AddOrderWithParticipant) {
    Writer w('F', 1, 0, kTs);
    w.u64(9).ch('S').u32(100).str("MSFT    ").u32(4000000).str("GSCO");
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<AddOrder>(r.got);
    EXPECT_EQ(m.h.type, 'F');
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(sv(m.mpid, 4), "GSCO");
}

TEST(Itch, OrderExecuted) {
    Writer w('E', 2, 0, kTs);
    w.u64(11).u32(50).u64(777);
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<OrderExecuted>(r.got);
    EXPECT_EQ(m.ref, 11u);
    EXPECT_EQ(m.shares, 50u);
    EXPECT_EQ(m.match, 777u);
}

TEST(Itch, OrderExecutedWithPrice) {
    Writer w('C', 2, 0, kTs);
    w.u64(11).u32(50).u64(778).ch('Y').u32(1234500);
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<OrderExecutedPrice>(r.got);
    EXPECT_EQ(m.match, 778u);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.price, 1234500u);
}

TEST(Itch, CancelDeleteReplace) {
    Recorder r;
    Writer x('X', 1, 0, kTs);
    x.u64(5).u32(20);
    ASSERT_EQ(decode(x, r), Status::Ok);
    EXPECT_EQ(std::get<OrderCancel>(r.got).cancelled, 20u);

    Writer d('D', 1, 0, kTs);
    d.u64(5);
    ASSERT_EQ(decode(d, r), Status::Ok);
    EXPECT_EQ(std::get<OrderDelete>(r.got).ref, 5u);

    Writer u('U', 1, 0, kTs);
    u.u64(5).u64(6).u32(80).u32(1000100);
    ASSERT_EQ(decode(u, r), Status::Ok);
    const auto& m = std::get<OrderReplace>(r.got);
    EXPECT_EQ(m.orig_ref, 5u);
    EXPECT_EQ(m.new_ref, 6u);
    EXPECT_EQ(m.shares, 80u);
    EXPECT_EQ(m.price, 1000100u);
}

TEST(Itch, Trade) {
    Writer w('P', 4, 0, kTs);
    w.u64(0).ch('B').u32(10).str("NVDA    ").u32(1300000).u64(42);
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<Trade>(r.got);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(sv(m.stock, 8), "NVDA    ");
    EXPECT_EQ(m.price, 1300000u);
    EXPECT_EQ(m.match, 42u);
}

TEST(Itch, CrossAndBroken) {
    Recorder r;
    Writer q('Q', 4, 0, kTs);
    q.u64(5'000'000'000ull).str("SPY     ").u32(5500000).u64(43).ch('O');
    ASSERT_EQ(decode(q, r), Status::Ok);
    const auto& c = std::get<CrossTrade>(r.got);
    EXPECT_EQ(c.shares, 5'000'000'000ull);
    EXPECT_EQ(c.cross_type, 'O');

    Writer b('B', 4, 0, kTs);
    b.u64(43);
    ASSERT_EQ(decode(b, r), Status::Ok);
    EXPECT_EQ(std::get<BrokenTrade>(r.got).match, 43u);
}

TEST(Itch, Imbalance) {
    Writer w('I', 4, 0, kTs);
    w.u64(1000).u64(250).ch('B').str("QQQ     ").u32(1).u32(2).u32(3).ch('C').ch('L');
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    const auto& m = std::get<Imbalance>(r.got);
    EXPECT_EQ(m.paired, 1000u);
    EXPECT_EQ(m.imbalance, 250u);
    EXPECT_EQ(m.direction, 'B');
    EXPECT_EQ(m.far_price, 1u);
    EXPECT_EQ(m.near_price, 2u);
    EXPECT_EQ(m.ref_price, 3u);
    EXPECT_EQ(m.cross_type, 'C');
    EXPECT_EQ(m.variation, 'L');
}

TEST(Itch, SystemDirectoryAndAction) {
    Recorder r;
    Writer s('S', 0, 0, kTs);
    s.ch('O');
    ASSERT_EQ(decode(s, r), Status::Ok);
    EXPECT_EQ(std::get<SystemEvent>(r.got).event, 'O');

    Writer d('R', 7, 0, kTs);
    d.str("AAPL    ")
        .ch('Q')
        .ch('N')
        .u32(100)
        .ch('N')
        .ch('C')
        .str("Z ")
        .ch('P')
        .ch('N')
        .ch(' ')
        .ch('1')
        .ch('N')
        .u32(0)
        .ch('N');
    ASSERT_EQ(decode(d, r), Status::Ok);
    const auto& m = std::get<StockDirectory>(r.got);
    EXPECT_EQ(sv(m.stock, 8), "AAPL    ");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.round_lot_size, 100u);
    EXPECT_EQ(sv(m.issue_subtype, 2), "Z ");
    EXPECT_EQ(m.luld_tier, '1');

    Writer h('H', 7, 0, kTs);
    h.str("AAPL    ").ch('T').ch(' ').str("    ");
    ASSERT_EQ(decode(h, r), Status::Ok);
    EXPECT_EQ(std::get<TradingAction>(r.got).state, 'T');
}

TEST(Itch, KnownButUnpackedTypeReportsHeader) {
    Writer w('Y', 9, 0, kTs);
    w.str("AAPL    ").ch('0');
    Recorder r;
    ASSERT_EQ(decode(w, r), Status::Ok);
    EXPECT_EQ(std::get<Header>(r.got).type, 'Y');
}

TEST(Itch, RejectsBadInput) {
    Recorder r;
    Writer w('D', 1, 0, kTs);
    w.u64(5);
    EXPECT_EQ(dispatch(w.b.data(), w.b.size() - 1, r), Status::Truncated);
    w.u8(0);
    EXPECT_EQ(dispatch(w.b.data(), w.b.size(), r), Status::BadLength);
    EXPECT_EQ(dispatch(w.b.data(), 0, r), Status::Truncated);
    const std::uint8_t junk[] = {'z', 0, 0};
    EXPECT_EQ(dispatch(junk, sizeof junk, r), Status::UnknownType);
}

TEST(Itch, Framing) {
    Writer d('D', 1, 0, kTs);
    d.u64(5);
    std::vector<std::uint8_t> buf = {0, static_cast<std::uint8_t>(d.b.size())};
    buf.insert(buf.end(), d.b.begin(), d.b.end());

    Frame f{};
    EXPECT_EQ(next_frame(buf.data(), 1, f), 0u);
    EXPECT_EQ(next_frame(buf.data(), buf.size() - 1, f), 0u);
    ASSERT_EQ(next_frame(buf.data(), buf.size(), f), buf.size());
    EXPECT_EQ(f.size, d.b.size());
    Recorder r;
    EXPECT_EQ(dispatch(f.data, f.size, r), Status::Ok);
}
