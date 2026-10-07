#pragma once

// Synthetic, spec-valid ITCH 5.0 order flow for a few symbols: adds around a drifting mid,
// partial and full executions, cancels, deletes and replaces, only ever against live
// orders; bids stay below the mid and asks above it. Driven by Philox, so a seed gives the same bytes on every compiler. Used for test
// fixtures; the queue-reactive AgentSource replaces it for research.

#include <cstdint>
#include <string>
#include <vector>

#include "core/philox.hpp"
#include "sources/itch_out.hpp"

namespace hft::sources {

class RandomFlow {
   public:
    RandomFlow(std::uint64_t seed, std::uint16_t symbols) : rng_(seed, 0, 0), syms_(symbols) {
        for (std::uint16_t i = 0; i < symbols; ++i)
            state_.push_back({static_cast<std::uint32_t>(20'0000 + 15'0000 * i), {}});
    }

    // Appends the whole day as a BinaryFILE stream: system start, directory, flow, end.
    void day(std::size_t messages, std::vector<std::uint8_t>& out) {
        ts_ = 4ull * 3600 * 1'000'000'000;
        system('O', out);
        for (std::uint16_t i = 0; i < syms_; ++i) directory(i, out);
        system('Q', out);
        for (std::size_t n = 0; n < messages; ++n) step(out);
        system('M', out);
        system('C', out);
    }

   private:
    struct Live {
        std::uint64_t ref;
        bool sell;
        std::uint32_t qty, px;
    };
    struct Sym {
        std::uint32_t mid;
        std::vector<Live> live;
    };

    std::uint32_t u(std::uint32_t purpose, std::uint32_t mod) {
        return rng_.draw(draw_++, purpose)[0] % mod;
    }

    void step(std::vector<std::uint8_t>& out) {
        ts_ += 1 + u(1, 2'000'000);
        const auto s = static_cast<std::uint16_t>(u(2, syms_));
        Sym& y = state_[s];
        if (u(3, 50) == 0) {
            y.mid = y.mid + 100 * (u(4, 2) ? 1u : 0u) - 100 * (u(5, 2) ? 1u : 0u);
            // A move executes the resting orders it crosses, so the book never stays crossed.
            for (std::size_t k = 0; k < y.live.size();) {
                const Live& l = y.live[k];
                if (l.sell ? l.px > y.mid : l.px < y.mid) {
                    ++k;
                    continue;
                }
                msg(out, 'E', s).u64(l.ref).u32(l.qty).u64(next_match_++);
                y.live[k] = y.live.back();
                y.live.pop_back();
            }
            return;
        }
        const std::uint32_t kind = y.live.size() < 5 ? 0 : u(6, 10);
        if (kind < 4) {  // add, one to ten ticks from the mid, occasionally far
            const bool sell = u(7, 2);
            const std::uint32_t ticks = u(8, 20) == 0 ? 200 + u(9, 3000) : 1 + u(10, 10);
            const std::uint32_t px =
                sell ? y.mid + ticks * 100 : y.mid - std::min(ticks * 100, y.mid - 100);
            const std::uint32_t qty = u(11, 4) == 0 ? 1 + u(12, 99) : 100 * (1 + u(13, 10));
            const Live l{next_ref_++, sell, qty, px};
            add(s, l, u(14, 10) == 0, out);
            y.live.push_back(l);
            return;
        }
        const std::size_t k = u(15, static_cast<std::uint32_t>(y.live.size()));
        Live& l = y.live[k];
        if (kind < 6) {  // execution, partial or full
            const std::uint32_t q = u(16, 2) ? l.qty : 1 + u(17, l.qty);
            msg(out, 'E', s).u64(l.ref).u32(q).u64(next_match_++);
            l.qty -= q;
        } else if (kind < 7 && l.qty > 1) {  // partial cancel
            const std::uint32_t q = 1 + u(18, l.qty - 1);
            msg(out, 'X', s).u64(l.ref).u32(q);
            l.qty -= q;
        } else if (kind < 9) {
            msg(out, 'D', s).u64(l.ref);
            l.qty = 0;
        } else {  // replace: new reference, size and price, same side
            const Live n{next_ref_++, l.sell, 100 * (1 + u(19, 5)),
                         l.sell ? y.mid + 100 * (1 + u(20, 5)) : y.mid - 100 * (1 + u(21, 5))};
            msg(out, 'U', s).u64(l.ref).u64(n.ref).u32(n.qty).u32(n.px);
            l = n;
        }
        if (l.qty == 0) {
            l = y.live.back();
            y.live.pop_back();
        }
    }

    BinaryFileWriter::Msg msg(std::vector<std::uint8_t>& out, char type, std::uint16_t locate) {
        return BinaryFileWriter::msg(out, type, locate, ts_);
    }

    static std::string name(std::uint16_t s) { return "SYM" + std::to_string(s); }

    void system(char event, std::vector<std::uint8_t>& out) {
        BinaryFileWriter::system(out, event, ts_);
    }

    void directory(std::uint16_t s, std::vector<std::uint8_t>& out) {
        BinaryFileWriter::directory(out, s, name(s), ts_);
    }

    void add(std::uint16_t s, const Live& l, bool attributed, std::vector<std::uint8_t>& out) {
        auto w = msg(out, attributed ? 'F' : 'A', s);
        w.u64(l.ref).u8(l.sell ? 'S' : 'B').u32(l.qty).text(name(s), 8).u32(l.px);
        if (attributed) w.text("ABCD", 4);
    }

    rng::Stream rng_;
    std::uint32_t draw_ = 0;
    std::uint16_t syms_;
    std::vector<Sym> state_;
    std::uint64_t ts_ = 0;
    std::uint64_t next_ref_ = 1;
    std::uint64_t next_match_ = 1;
};

}  // namespace hft::sources
