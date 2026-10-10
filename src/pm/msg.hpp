#pragma once

// Typed view of the exchange's market-channel messages, decoded from a JsonTape without
// allocation once the buffers have grown. Strings are views into the message text.

#include <array>
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/tape.hpp"
#include "pm/book.hpp"

namespace hft::pm {

// Leading decimal number of `s`, 0 if there is none (strtod without its exotic forms).
inline double parse_size(std::string_view s) {
    // digits[.digits] with at most 15 digits: mantissa and power of ten are exact doubles, so
    // one division rounds correctly, as from_chars does.
    static constexpr double kPow10[] = {1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    std::uint64_t m = 0;
    int digits = 0, frac = -1;
    for (const char c : s) {
        if (c >= '0' && c <= '9') m = m * 10 + static_cast<std::uint64_t>(c - '0'), ++digits;
        else if (c == '.' && frac < 0) frac = digits;
        else digits = 99;
    }
    if (digits > 0 && digits <= 15 && frac != digits && s[0] != '.')
        return static_cast<double>(m) / kPow10[frac < 0 ? 0 : digits - frac];
    double v = 0;
    const char* b = s.data() + (!s.empty() && s[0] == '+');
    const auto r = std::from_chars(b, s.data() + s.size(), v);
    return r.ptr == b ? 0.0 : v;
}

enum class Kind : std::uint8_t { Book, PriceChange, Trade, Other };

struct Level {
    std::int32_t px;
    double size;
};

struct Change {
    std::string_view asset;
    std::int32_t px;
    double size;
    bool buy;
    std::string_view best_bid, best_ask;  // the exchange's best prices after the message
};

struct Event {
    Kind kind = Kind::Other;
    std::string_view type, asset;
    std::span<const Level> bids, asks;     // Book
    std::int32_t tick = -1;                // Book: tick_size, -1 if absent
    std::span<const Change> changes;       // PriceChange
    bool has_changes = false;              // PriceChange carried a price_changes list
    std::int32_t px = -1;                  // Trade
    std::string_view price;                // Trade: price as sent
    double size = 0;                       // Trade
    bool buy = false;                      // Trade: the aggressor bought
    std::int64_t exch_ms = 0;              // exchange timestamp, ms since epoch, 0 if absent
    std::int32_t conn = -1;                // control records ("_heartbeat", "_reconnect"): connection
};

class Decoder {
   public:
    // Calls fn(const Event&) for the message's event, or each event of a batch. False if
    // the text is not JSON. Messages in the shape the exchange sends take a single pass over
    // the bytes; anything else (escapes, deep nesting, unusual numbers) goes through the tape,
    // which gives the same events.
    template <class Fn>
    bool decode(std::string_view text, Fn&& fn) {
        if (!fast(text)) return ++fallbacks_, decode_tape(text, fn);
        for (Pending& e : pend_) {
            e.ev.bids = std::span<const Level>(lv_).subspan(e.bids.b, e.bids.e - e.bids.b);
            e.ev.asks = std::span<const Level>(lv_).subspan(e.asks.b, e.asks.e - e.asks.b);
            e.ev.changes = std::span<const Change>(ch_).subspan(e.chg.b, e.chg.e - e.chg.b);
            fn(static_cast<const Event&>(e.ev));
        }
        return true;
    }

    // Messages that took the tape.
    std::uint64_t fallbacks() const { return fallbacks_; }

    template <class Fn>
    bool decode_tape(std::string_view text, Fn&& fn) {
        if (!t_.parse(text)) return false;
        const std::uint32_t r = t_.root();
        if (t_.type(r) == net::JsonTape::Type::Arr) {
            for (std::uint32_t e = t_.first(r); e != t_.end(r); e = t_.next(e)) fn(event(e));
        } else {
            fn(event(r));
        }
        return true;
    }

   private:
    using Tape = net::JsonTape;
    enum Field { EventType, AssetId, Price, Size, Side, BestBid, BestAsk, TickSize, Bids, Asks, PriceChanges, Timestamp, Conn, kFields };
    using Fields = std::array<std::uint32_t, kFields>;

    // Little-endian integer of the first n bytes of a literal, to compare keys a word at a time.
    static constexpr std::uint64_t lit(const char* s, int n) {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= std::uint64_t{static_cast<unsigned char>(s[i])} << (8 * i);
        return v;
    }
    template <class T>
    static T load(const char* p) {
        T v;
        std::memcpy(&v, p, sizeof v);
        return v;
    }

    // Keys are matched as sent; the exchange does not escape them.
    static int field(std::string_view k) {
        const char* p = k.data();
        switch (k.size()) {
            case 4: {
                const auto w = load<std::uint32_t>(p);
                return w == lit("side", 4) ? Side : w == lit("size", 4) ? Size : w == lit("bids", 4) ? Bids
                     : w == lit("asks", 4) ? Asks : w == lit("conn", 4) ? Conn : -1;
            }
            case 5: return load<std::uint32_t>(p) == lit("pric", 4) && p[4] == 'e' ? Price : -1;
            case 8: {
                const auto w = load<std::uint64_t>(p);
                return w == lit("asset_id", 8) ? AssetId : w == lit("best_bid", 8) ? BestBid : w == lit("best_ask", 8) ? BestAsk : -1;
            }
            case 9: {
                const auto w = load<std::uint64_t>(p);
                return w == lit("tick_siz", 8) && p[8] == 'e' ? TickSize : w == lit("timestam", 8) && p[8] == 'p' ? Timestamp : -1;
            }
            case 10: return load<std::uint64_t>(p) == lit("event_ty", 8) && load<std::uint16_t>(p + 8) == lit("pe", 2) ? EventType : -1;
            case 13:
                return load<std::uint64_t>(p) == lit("price_ch", 8) && load<std::uint32_t>(p + 8) == lit("ange", 4) && p[12] == 's'
                           ? PriceChanges
                           : -1;
            default: return -1;
        }
    }

    // Value node of each known member of `obj` (the first, if repeated), npos if absent.
    Fields fields(std::uint32_t obj) const {
        Fields f;
        f.fill(Tape::npos);
        if (t_.type(obj) != Tape::Type::Obj) return f;
        for (std::uint32_t c = t_.first(obj); c != t_.end(obj); c = t_.next(c + 1))
            if (const int i = field(t_.raw(c)); i >= 0 && f[static_cast<std::size_t>(i)] == Tape::npos)
                f[static_cast<std::size_t>(i)] = c + 1;
        return f;
    }
    std::string_view str(const Fields& f, Field i) const {
        const std::uint32_t v = f[i];
        return v != Tape::npos && t_.type(v) == Tape::Type::Str ? t_.raw(v) : std::string_view();
    }

    void levels(std::uint32_t a, std::vector<Level>& out) {
        out.clear();
        if (a == Tape::npos || t_.type(a) != Tape::Type::Arr) return;
        for (std::uint32_t l = t_.first(a); l != t_.end(a); l = t_.next(l)) {
            const Fields f = fields(l);
            out.push_back({parse_price(str(f, Price)), parse_size(str(f, Size))});
        }
    }

    Event event(std::uint32_t e) {
        Event ev;
        const Fields f = fields(e);
        ev.type = str(f, EventType);
        if (const std::string_view ts = str(f, Timestamp); !ts.empty()) std::from_chars(ts.data(), ts.data() + ts.size(), ev.exch_ms);
        if (!ev.type.empty() && ev.type[0] == '_' && f[Conn] != Tape::npos) ev.conn = static_cast<std::int32_t>(t_.num(f[Conn]));
        if (ev.type == "book") {
            ev.kind = Kind::Book;
            ev.asset = str(f, AssetId);
            levels(f[Bids], bids_);
            levels(f[Asks], asks_);
            ev.bids = bids_, ev.asks = asks_;
            ev.tick = parse_price(str(f, TickSize));
        } else if (ev.type == "price_change") {
            ev.kind = Kind::PriceChange;
            changes_.clear();
            const std::uint32_t a = f[PriceChanges];
            ev.has_changes = a != Tape::npos;
            if (a != Tape::npos && t_.type(a) == Tape::Type::Arr)
                for (std::uint32_t c = t_.first(a); c != t_.end(a); c = t_.next(c)) {
                    const Fields g = fields(c);
                    changes_.push_back({str(g, AssetId), parse_price(str(g, Price)), parse_size(str(g, Size)),
                                        str(g, Side) == "BUY", str(g, BestBid), str(g, BestAsk)});
                }
            ev.changes = changes_;
        } else if (ev.type == "last_trade_price") {
            ev.kind = Kind::Trade;
            ev.asset = str(f, AssetId);
            ev.price = str(f, Price);
            ev.px = parse_price(ev.price);
            ev.size = parse_size(str(f, Size));
            ev.buy = str(f, Side) == "BUY";
        }
        return ev;
    }

    // ---- single-pass path. Every failure means "not handled here", never "malformed": the
    // tape decides. Events are buffered so a late failure has emitted nothing.
    struct Range {
        std::uint32_t b = 0, e = 0;
    };
    struct Pending {
        Event ev;
        Range bids, asks, chg;
    };
    // First occurrence of each known member: whether it was seen, and its contents if a string.
    // s[f] is written only for string members and read only through str(), so it is left
    // uninitialised: zeroing it was a measurable share of each object's cost.
    struct Members {
        struct Sv {
            const char* p;
            std::size_t n;
            operator std::string_view() const { return {p, n}; }
        };
        std::uint32_t seen = 0, strs = 0;
        Sv s[kFields];
        bool first(int f) {
            if (f < 0 || (seen >> f & 1)) return false;
            seen |= 1u << f;
            return true;
        }
        bool has(int f) const { return seen >> f & 1; }
        std::string_view str(int f) const { return strs >> f & 1 ? s[f] : std::string_view(); }
        bool set(int f, std::string_view v) { return s[f] = {v.data(), v.size()}, strs |= 1u << f, true; }
    };

    static constexpr int kMaxDepth = 16;

    static bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    void ws() {
        while (p_ != e_ && blank(*p_)) ++p_;
    }
    bool eat(char c) {
        if (p_ == e_ || *p_ != c) return false;
        ++p_;
        return true;
    }
    char peek() const { return p_ != e_ ? *p_ : '\0'; }

    // First '"' or '\\' at or after p, eight bytes at a time; the lowest flagged byte of the
    // zero-byte test is exact, so ctz finds it.
    static const char* quote_or_escape(const char* p, const char* e) {
#ifdef __AVX2__
        const __m256i qv = _mm256_set1_epi8('"'), bv = _mm256_set1_epi8('\\');
        for (; e - p >= 32; p += 32) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            if (const auto m = static_cast<std::uint32_t>(
                    _mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(x, qv), _mm256_cmpeq_epi8(x, bv)))))
                return p + __builtin_ctz(m);
        }
#endif
        constexpr std::uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
        for (; e - p >= 8; p += 8) {
            std::uint64_t w;
            std::memcpy(&w, p, 8);
            const std::uint64_t q = w ^ (ones * '"'), b = w ^ (ones * '\\');
            if (const std::uint64_t m = (((q - ones) & ~q) | ((b - ones) & ~b)) & highs) return p + (__builtin_ctzll(m) >> 3);
        }
        while (p != e && *p != '"' && *p != '\\') ++p;
        return p;
    }

    bool string(std::string_view& out) {
        if (!eat('"')) return false;
        const char* q = quote_or_escape(p_, e_);
        if (q == e_ || *q != '"') return false;
        out = {p_, static_cast<std::size_t>(q - p_)};
        p_ = q + 1;
        return true;
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }
    // -?digits(.digits)?([eE][+-]?digits)?, which the tape always accepts; `out` is set for a
    // plain integer of at most nine digits.
    bool number(std::int64_t* out = nullptr, bool* plain = nullptr) {
        const bool neg = eat('-');
        const char* d = p_;
        while (p_ != e_ && digit(*p_)) ++p_;
        if (p_ == d) return false;
        const char* int_end = p_;
        if (eat('.')) {
            const char* f = p_;
            while (p_ != e_ && digit(*p_)) ++p_;
            if (p_ == f) return false;
        }
        if (p_ != e_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ != e_ && (*p_ == '+' || *p_ == '-')) ++p_;
            const char* x = p_;
            while (p_ != e_ && digit(*p_)) ++p_;
            if (p_ == x) return false;
        }
        if (plain) {
            *plain = p_ == int_end && int_end - d <= 9;
            if (*plain) {
                std::int64_t v = 0;
                for (const char* c = d; c != int_end; ++c) v = v * 10 + (*c - '0');
                *out = neg ? -v : v;
            }
        }
        return true;
    }

    bool literal(std::string_view w) {
        if (static_cast<std::size_t>(e_ - p_) < w.size() || std::memcmp(p_, w.data(), w.size()) != 0) return false;
        p_ += w.size();
        return true;
    }

    bool skip(int depth) {
        if (depth > kMaxDepth) return false;
        switch (peek()) {
            case '"': {
                std::string_view s;
                return string(s);
            }
            case '{':
            case '[': {
                const char close = *p_ == '{' ? '}' : ']';
                const bool obj = *p_++ == '{';
                ws();
                if (eat(close)) return true;
                for (;;) {
                    if (obj) {
                        std::string_view k;
                        if (!string(k)) return false;
                        ws();
                        if (!eat(':')) return false;
                        ws();
                    }
                    if (!skip(depth + 1)) return false;
                    ws();
                    if (eat(close)) return true;
                    if (!eat(',')) return false;
                    ws();
                }
            }
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default: return number();
        }
    }

    bool string_into(Members& m, int f) {
        std::string_view v;
        return string(v) && m.set(f, v);
    }

    // Value of a member: a string goes into m when it is the member's first occurrence.
    bool member_value(Members& m, int f, int depth) {
        const bool first = m.first(f);
        if (first && peek() == '"') return string_into(m, f);
        return skip(depth);
    }

    // Object members, calling on(field, first) at each value; on() consumes the value.
    template <class On>
    bool object(On&& on) {
        if (!eat('{')) return false;
        ws();
        if (eat('}')) return true;
        for (;;) {
            std::string_view k;
            if (!string(k)) return false;
            ws();
            if (!eat(':')) return false;
            ws();
            if (!on(field(k))) return false;
            ws();
            if (eat('}')) return true;
            if (!eat(',')) return false;
            ws();
        }
    }

    // Array of objects, calling elem() at each '{'.
    template <class Elem>
    bool array(Elem&& elem) {
        if (!eat('[')) return false;
        ws();
        if (eat(']')) return true;
        for (;;) {
            if (peek() != '{' || !elem()) return false;
            ws();
            if (eat(']')) return true;
            if (!eat(',')) return false;
            ws();
        }
    }

    bool levels_fast(Range& r) {
        r.b = static_cast<std::uint32_t>(lv_.size());
        const bool ok = array([&] {
            Members m;
            if (!object([&](int f) { return member_value(m, f, 3); })) return false;
            lv_.push_back({parse_price(m.str(Price)), parse_size(m.str(Size))});
            return true;
        });
        r.e = static_cast<std::uint32_t>(lv_.size());
        return ok;
    }

    bool changes_fast(Range& r) {
        r.b = static_cast<std::uint32_t>(ch_.size());
        const bool ok = array([&] {
            Members m;
            if (!object([&](int f) { return member_value(m, f, 3); })) return false;
            ch_.push_back({m.str(AssetId), parse_price(m.str(Price)), parse_size(m.str(Size)), m.str(Side) == "BUY", m.str(BestBid), m.str(BestAsk)});
            return true;
        });
        r.e = static_cast<std::uint32_t>(ch_.size());
        return ok;
    }

    bool event_fast() {
        Members m;
        Pending& pe = pend_.emplace_back();
        bool bids_arr = false, asks_arr = false, chg_arr = false, conn_plain = false;
        std::int64_t conn = 0;
        const bool ok = object([&](int f) {
            const bool first = m.first(f);
            if (!first) return skip(2);
            switch (f) {
                case Bids: return peek() == '[' ? (bids_arr = true, levels_fast(pe.bids)) : skip(2);
                case Asks: return peek() == '[' ? (asks_arr = true, levels_fast(pe.asks)) : skip(2);
                case PriceChanges: return peek() == '[' ? (chg_arr = true, changes_fast(pe.chg)) : skip(2);
                case Conn:
                    if (peek() == '-' || digit(peek())) return number(&conn, &conn_plain);
                    return skip(2);
                default:
                    if (peek() == '"') return string_into(m, f);
                    return skip(2);
            }
        });
        if (!ok) return false;
        Event& ev = pe.ev;
        ev.type = m.str(EventType);
        if (const std::string_view ts = m.str(Timestamp); !ts.empty()) std::from_chars(ts.data(), ts.data() + ts.size(), ev.exch_ms);
        if (!ev.type.empty() && ev.type[0] == '_' && m.has(Conn)) {
            if (!conn_plain) return false;
            ev.conn = static_cast<std::int32_t>(conn);
        }
        if (!bids_arr) pe.bids = {};
        if (!asks_arr) pe.asks = {};
        if (!chg_arr) pe.chg = {};
        if (ev.type == "book") {
            ev.kind = Kind::Book;
            ev.asset = m.str(AssetId);
            ev.tick = parse_price(m.str(TickSize));
            pe.chg = {};
        } else if (ev.type == "price_change") {
            ev.kind = Kind::PriceChange;
            ev.has_changes = m.has(PriceChanges);
            pe.bids = pe.asks = {};
        } else {
            pe.bids = pe.asks = pe.chg = {};
            if (ev.type == "last_trade_price") {
                ev.kind = Kind::Trade;
                ev.asset = m.str(AssetId);
                ev.price = m.str(Price);
                ev.px = parse_price(ev.price);
                ev.size = parse_size(m.str(Size));
                ev.buy = m.str(Side) == "BUY";
            }
        }
        return true;
    }

    bool fast(std::string_view text) {
        p_ = text.data(), e_ = text.data() + text.size();
        pend_.clear(), lv_.clear(), ch_.clear();
        ws();
        if (peek() == '[') {
            ++p_;
            ws();
            if (!eat(']')) {
                for (;;) {
                    if (peek() != '{' || !event_fast()) return false;
                    ws();
                    if (eat(']')) break;
                    if (!eat(',')) return false;
                    ws();
                }
            }
        } else if (peek() != '{' || !event_fast()) {
            return false;
        }
        ws();
        return p_ == e_;
    }

    Tape t_;
    std::vector<Level> bids_, asks_;
    std::vector<Change> changes_;
    const char *p_ = nullptr, *e_ = nullptr;
    std::uint64_t fallbacks_ = 0;
    std::vector<Pending> pend_;
    std::vector<Level> lv_;
    std::vector<Change> ch_;
};

// Dense indices for token ids: open addressing on a hash of the id's ends.
class TokenIndex {
   public:
    static constexpr std::uint32_t npos = ~0u;

    // Index of `id`, assigning the next one if it is new.
    std::uint32_t get(std::string_view id) {
        if ((keys_.size() + 1) * 2 > slots_.size()) grow();
        for (std::size_t i = hash(id) & mask_;; i = (i + 1) & mask_) {
            if (slots_[i] == npos) {
                slots_[i] = static_cast<std::uint32_t>(keys_.size());
                keys_.emplace_back(id);
                return slots_[i];
            }
            if (keys_[slots_[i]] == id) return slots_[i];
        }
    }
    std::uint32_t find(std::string_view id) const {
        if (slots_.empty()) return npos;
        for (std::size_t i = hash(id) & mask_;; i = (i + 1) & mask_) {
            if (slots_[i] == npos || keys_[slots_[i]] == id) return slots_[i];
        }
    }
    const std::string& key(std::uint32_t i) const { return keys_[i]; }
    std::size_t size() const { return keys_.size(); }

   private:
    static std::uint64_t load8(const char* p) {
        std::uint64_t v;
        __builtin_memcpy(&v, p, 8);
        return v;
    }
    // Ids are decimal renderings of random 256-bit numbers, so their ends are well mixed.
    static std::uint64_t hash(std::string_view s) {
        std::uint64_t h = s.size();
        if (s.size() >= 8) h ^= load8(s.data()) * 0x9E3779B97F4A7C15ull ^ load8(s.data() + s.size() - 8);
        else
            for (const char c : s) h = h * 131 + static_cast<unsigned char>(c);
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        return h ^ (h >> 32);
    }
    void grow() {
        const std::size_t n = slots_.empty() ? 64 : slots_.size() * 2;
        slots_.assign(n, npos);
        mask_ = n - 1;
        for (std::uint32_t k = 0; k < keys_.size(); ++k)
            for (std::size_t i = hash(keys_[k]) & mask_;; i = (i + 1) & mask_)
                if (slots_[i] == npos) {
                    slots_[i] = k;
                    break;
                }
    }
    std::vector<std::uint32_t> slots_;
    std::vector<std::string> keys_;
    std::size_t mask_ = 0;
};

}  // namespace hft::pm
