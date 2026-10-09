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
};

class Decoder {
   public:
    // Calls fn(const Event&) for the message's event, or each event of a batch. False if
    // the text is not JSON.
    template <class Fn>
    bool decode(std::string_view text, Fn&& fn) {
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
    enum Field { EventType, AssetId, Price, Size, Side, BestBid, BestAsk, TickSize, Bids, Asks, PriceChanges, kFields };
    using Fields = std::array<std::uint32_t, kFields>;

    // Keys are matched as sent; the exchange does not escape them.
    static int field(std::string_view k) {
        switch (k.size()) {
            case 4: return k == "side" ? Side : k == "size" ? Size : k == "bids" ? Bids : k == "asks" ? Asks : -1;
            case 5: return k == "price" ? Price : -1;
            case 8: return k == "asset_id" ? AssetId : k == "best_bid" ? BestBid : k == "best_ask" ? BestAsk : -1;
            case 9: return k == "tick_size" ? TickSize : -1;
            case 10: return k == "event_type" ? EventType : -1;
            case 13: return k == "price_changes" ? PriceChanges : -1;
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

    Tape t_;
    std::vector<Level> bids_, asks_;
    std::vector<Change> changes_;
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
