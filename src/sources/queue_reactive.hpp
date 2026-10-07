#pragma once

// Queue-reactive order flow (Huang, Lehalle and Rosenbaum 2015, model I) for one symbol,
// written as ITCH 5.0 so the replay exchange and backtest run on it unchanged.
//
// State: K levels per side around a reference price p_ref (half a tick off the price grid),
// level i at p_ref -/+ (i - 1/2) tick, each holding n_i orders of `aes` shares. Per level i and
// queue size n: limit insertions at rate L_i(n), cancellations of one random order at C_i(n),
// and market orders taking the front order at M_i(n) when level i is the best non-empty level
// of its side; the same rates on both sides. When level 1 of a side empties, with probability
// theta p_ref moves one tick towards it: the levels shift, the emptied price becomes the first
// level of the other side and every new level is redrawn from `init` (level 1 conditioned on
// at least one order); the level leaving the window is deleted.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/philox.hpp"
#include "sources/itch_out.hpp"

namespace hft::sources {

struct QrParams {
    int K = 3, N = 30;  // levels per side; queue sizes 0..N orders
    std::uint32_t aes = 100, tick = 100;
    std::uint32_t p_ref = 200'050;  // price units of 1e-4 dollars, between two ticks
    double theta = 0.5;
    // Rates per second, index i * (N + 1) + n for level i = 0..K-1.
    std::vector<double> L, C, M;
    std::vector<double> init;  // redraw distribution, same indexing, rows sum to one
    std::uint64_t start_ns = 34'200'000'000'000ull, end_ns = 57'600'000'000'000ull;
};

class QueueReactive {
   public:
    QueueReactive(QrParams p, std::uint64_t seed) : p_(std::move(p)), rng_(seed, 1, 0) {
        const auto cells = static_cast<std::size_t>(p_.K * (p_.N + 1));
        if (p_.L.size() != cells || p_.C.size() != cells || p_.M.size() != cells ||
            p_.init.size() != cells || p_.p_ref % p_.tick != p_.tick / 2 ||
            p_.p_ref <= static_cast<std::uint32_t>(p_.K + 1) * p_.tick)
            throw std::invalid_argument("QrParams: table sizes or reference price");
        for (auto& side : q_) side.assign(static_cast<std::size_t>(p_.K), {});
    }

    // The whole session as a BinaryFILE stream for locate 1.
    void day(std::vector<std::uint8_t>& out) {
        ts_ = p_.start_ns - 1'000'000'000ull;
        BinaryFileWriter::system(out, 'O', ts_);
        BinaryFileWriter::directory(out, 1, "QRSIM", ts_);
        BinaryFileWriter::system(out, 'Q', ts_);
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < p_.K; ++i) refill(s, i, i == 0, out);
        ts_ = p_.start_ns;
        while (step(out)) {
        }
        ts_ = p_.end_ns;
        BinaryFileWriter::system(out, 'M', ts_);
        BinaryFileWriter::system(out, 'C', ts_);
    }

    std::uint32_t p_ref() const { return p_ref_; }
    int queue(int side, int level) const { return static_cast<int>(q_[side][level].size()); }
    std::uint64_t events() const { return events_; }
    std::uint64_t moves() const { return moves_; }

   private:
    double uniform() {
        const auto w = rng_.draw(draw_++, 0);
        return ((static_cast<std::uint64_t>(w[0]) << 21) ^ (w[1] >> 11)) * 0x1p-53;
    }
    std::size_t cell(int i, int n) const { return static_cast<std::size_t>(i * (p_.N + 1) + n); }
    std::uint32_t price(int s, int i) const {
        const std::uint32_t off = static_cast<std::uint32_t>(i) * p_.tick + p_.tick / 2;
        return s == 0 ? p_ref_ - off : p_ref_ + off;
    }
    bool best(int s, int i) const {
        for (int j = 0; j < i; ++j)
            if (!q_[s][j].empty()) return false;
        return true;
    }
    double rate(int s, int i, int type) const {
        const int n = static_cast<int>(q_[s][i].size());
        if (type == 0) return n < p_.N ? p_.L[cell(i, n)] : 0.0;
        if (n == 0) return 0.0;
        if (type == 1) return p_.C[cell(i, n)];
        return best(s, i) ? p_.M[cell(i, n)] : 0.0;
    }

    BinaryFileWriter::Msg msg(std::vector<std::uint8_t>& out, char type) {
        return BinaryFileWriter::msg(out, type, 1, ts_);
    }
    void add(int s, int i, std::vector<std::uint8_t>& out) {
        const std::uint64_t ref = next_ref_++;
        msg(out, 'A').u64(ref).u8(s == 0 ? 'B' : 'S').u32(p_.aes).text("QRSIM", 8).u32(price(s, i));
        q_[s][i].push_back(ref);
    }
    void refill(int s, int i, bool nonempty, std::vector<std::uint8_t>& out) {
        double u = uniform(), mass = 0;
        for (int n = nonempty ? 1 : 0; n <= p_.N; ++n) mass += p_.init[cell(i, n)];
        int n = nonempty ? 1 : 0;
        for (u *= mass; n < p_.N && u >= p_.init[cell(i, n)]; ++n) u -= p_.init[cell(i, n)];
        for (int k = 0; k < n; ++k) add(s, i, out);
    }

    // Level 1 of side s emptied: with probability theta the reference moves one tick to it.
    void depleted(int s, std::vector<std::uint8_t>& out) {
        if (uniform() >= p_.theta) return;
        ++moves_;
        const int o = 1 - s, K = p_.K;
        // The new first level is written before the old last level is deleted, so a reader
        // sees the reference move with the first insertion and the deletions outside its window.
        std::vector<std::uint64_t> leaving = std::move(q_[o][K - 1]);
        for (int i = K - 1; i > 0; --i) q_[o][i] = std::move(q_[o][i - 1]);
        q_[o][0].clear();
        for (int i = 0; i + 1 < K; ++i) q_[s][i] = std::move(q_[s][i + 1]);
        q_[s][K - 1].clear();
        p_ref_ = s == 0 ? p_ref_ - p_.tick : p_ref_ + p_.tick;
        refill(o, 0, true, out);
        for (const std::uint64_t ref : leaving) msg(out, 'D').u64(ref);
        refill(s, K - 1, false, out);
    }

    bool step(std::vector<std::uint8_t>& out) {
        double total = 0;
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < p_.K; ++i)
                for (int t = 0; t < 3; ++t) total += rate(s, i, t);
        if (total <= 0) return false;
        const double wait = -std::log1p(-uniform()) / total;
        const double next = static_cast<double>(ts_) + wait * 1e9;
        if (next >= static_cast<double>(p_.end_ns)) return false;
        ts_ = std::max(ts_ + 1, static_cast<std::uint64_t>(next));
        double u = uniform() * total;
        int s = 0, i = 0, t = 0;
        for (int k = 0; k < 2 * p_.K * 3; ++k) {
            s = k / (3 * p_.K), i = (k / 3) % p_.K, t = k % 3;
            const double r = rate(s, i, t);
            if (u < r) break;
            u -= r;
        }
        ++events_;
        auto& lv = q_[s][i];
        if (t == 0) {
            add(s, i, out);
            return true;
        }
        if (t == 1) {
            const auto k = static_cast<std::size_t>(uniform() * static_cast<double>(lv.size()));
            msg(out, 'D').u64(lv[k]);
            lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            msg(out, 'E').u64(lv.front()).u32(p_.aes).u64(next_match_++);
            lv.erase(lv.begin());
        }
        if (i == 0 && lv.empty()) depleted(s, out);
        return true;
    }

    QrParams p_;
    rng::Stream rng_;
    std::uint32_t draw_ = 0;
    std::uint32_t p_ref_ = p_.p_ref;
    std::vector<std::vector<std::uint64_t>> q_[2];
    std::uint64_t ts_ = 0, next_ref_ = 1, next_match_ = 1, events_ = 0, moves_ = 0;
};

}  // namespace hft::sources
