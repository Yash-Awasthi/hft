#pragma once

// Positions, cash, fees and PnL in exact integer units (types.hpp). Positions are long only:
// the venue sells only shares held, and a short view is a long position in the other outcome.
// Cost basis is integer and a sale removes cost in proportion, so
//   cash - capital + cost == realised - fees
// holds exactly after every operation (identity_holds). Fills count when matched; a failed
// settlement is applied as the opposite fill with the fee refunded (DESIGN D16, L1).

#include <cstdint>
#include <optional>
#include <vector>

#include "book/id_map.hpp"
#include "core/pool.hpp"
#include "exec/types.hpp"

namespace hft::exec {

class Ledger {
   public:
    struct Fill {
        std::uint32_t token;
        Side side;
        Px px;
        Qty qty;
        Usd fee;
    };

    explicit Ledger(Usd capital) : capital_(capital), cash_(capital), pending_(64), ids_(256) {}

    // Setup only: per-token state for tokens [0, n).
    void ensure(std::uint32_t n) {
        if (n > toks_.size()) toks_.resize(n);
    }

    // A matched fill. `fill_id` must be below 2^40 and unique among pending fills.
    void fill(std::uint64_t fill_id, std::uint32_t t, Side side, Px px, Qty q, Usd fee) {
        apply(t, side, px, q, fee);
        std::uint32_t i;
        if (free_ != book::kNoOrder) {
            i = free_;
            free_ = next_free_[i];
        } else {
            i = used_++;
            pending_.reserve(used_);
            next_free_.resize(used_);
        }
        pending_[i] = {t, side, px, q, fee};
        ids_.insert(fill_id, i);
        ++n_pending_;
    }
    // Settlement of a pending fill; false if the id is not pending.
    bool settled(std::uint64_t fill_id) {
        const Fill* f = take(fill_id);
        if (!f) return false;
        toks_[f->token].confirmed += f->side == Side::Buy ? f->qty : -f->qty;
        return true;
    }
    bool failed(std::uint64_t fill_id) {
        const Fill* f = take(fill_id);
        if (!f) return false;
        apply(f->token, f->side == Side::Buy ? Side::Sell : Side::Buy, f->px, f->qty, -f->fee);
        return true;
    }
    const Fill* pending(std::uint64_t fill_id) const {
        const std::uint32_t i = ids_.find(fill_id);
        return i == book::kNoOrder ? nullptr : &pending_[i];
    }

    // n Yes + n No -> n USD, and the reverse; the proceeds or cost are split evenly between legs.
    void merge(std::uint32_t yes, std::uint32_t no, Qty n) {
        apply(yes, Side::Sell, kPxOne / 2, n, 0);
        apply(no, Side::Sell, kPxOne / 2, n, 0);
    }
    void split(std::uint32_t yes, std::uint32_t no, Qty n) {
        apply(yes, Side::Buy, kPxOne / 2, n, 0);
        apply(no, Side::Buy, kPxOne / 2, n, 0);
    }
    // The market resolved: the position pays 1 or 0 per share.
    void resolve(std::uint32_t t, bool won) {
        if (toks_[t].pos > 0) apply(t, Side::Sell, won ? kPxOne : 0, toks_[t].pos, 0);
    }

    void reserve_cash(Usd u) { reserved_cash_ += u; }
    void release_cash(Usd u) { reserved_cash_ -= u; }
    void reserve_pos(std::uint32_t t, Qty q) { toks_[t].reserved += q; }
    void release_pos(std::uint32_t t, Qty q) { toks_[t].reserved -= q; }

    // Capital time: money in positions and reserved for open buys, times elapsed time.
    void tick(Ns now) {
        if (have_tick_) {
            Usd locked = reserved_cash_;
            for (const Tok& k : toks_) locked += k.cost > 0 ? k.cost : 0;
            capital_ns_ += static_cast<__int128>(locked) * (now - last_tick_);
        }
        have_tick_ = true;
        last_tick_ = now;
    }

    Usd capital() const { return capital_; }
    Usd cash() const { return cash_; }
    Usd available_cash() const { return cash_ - reserved_cash_; }
    Qty pos(std::uint32_t t) const { return toks_[t].pos; }
    Qty available_pos(std::uint32_t t) const { return toks_[t].pos - toks_[t].reserved; }
    Qty confirmed(std::uint32_t t) const { return toks_[t].confirmed; }
    Usd cost(std::uint32_t t) const { return toks_[t].cost; }
    Usd realised() const { return realised_; }
    Usd fees() const { return fees_; }
    std::uint64_t deficits() const { return deficits_; }
    std::size_t pending_fills() const { return n_pending_; }
    std::uint32_t tokens() const { return static_cast<std::uint32_t>(toks_.size()); }

    // marks[t]: price each position is valued at.
    Usd unrealised(const Px* marks) const {
        Usd u = 0;
        for (std::uint32_t t = 0; t < toks_.size(); ++t) u += notional(marks[t], toks_[t].pos) - toks_[t].cost;
        return u;
    }
    Usd total(const Px* marks) const { return realised_ - fees_ + unrealised(marks); }
    bool identity_holds() const {
        Usd cost = 0;
        for (const Tok& k : toks_) cost += k.cost;
        return cash_ - capital_ + cost == realised_ - fees_;
    }
    double capital_usd_days() const {
        return static_cast<double>(capital_ns_) / (static_cast<double>(kDollar) * 86'400e9);
    }

   private:
    struct Tok {
        Qty pos = 0, confirmed = 0, reserved = 0;
        Usd cost = 0;
    };

    void apply(std::uint32_t t, Side side, Px px, Qty q, Usd fee) {
        Tok& k = toks_[t];
        const Usd amount = notional(px, q);
        fees_ += fee;
        if (side == Side::Buy) {
            cash_ -= amount + fee;
            k.cost += amount;
            k.pos += q;
            return;
        }
        // Cost leaves in proportion to the shares sold; selling more than is held (only a failed
        // settlement can do that) takes all the cost and is a deficit.
        Usd removed = k.cost;
        if (q <= k.pos) removed = static_cast<Usd>(static_cast<__int128>(k.cost) * q / k.pos);
        else ++deficits_;
        cash_ += amount - fee;
        realised_ += amount - removed;
        k.cost -= removed;
        k.pos -= q;
    }

    const Fill* take(std::uint64_t fill_id) {
        const book::IdProbe p = ids_.probe(fill_id);
        if (p.val == book::kNoOrder) return nullptr;
        ids_.erase_at(p.at);
        next_free_[p.val] = free_;
        free_ = p.val;
        --n_pending_;
        return &pending_[p.val];
    }

    Usd capital_, cash_, reserved_cash_ = 0, realised_ = 0, fees_ = 0;
    std::vector<Tok> toks_;
    Pool<Fill> pending_;
    std::vector<std::uint32_t> next_free_;
    book::LinearMap ids_;
    std::uint32_t used_ = 0, free_ = book::kNoOrder;
    std::size_t n_pending_ = 0;
    std::uint64_t deficits_ = 0;
    __int128 capital_ns_ = 0;
    Ns last_tick_ = 0;
    bool have_tick_ = false;
};

}  // namespace hft::exec
