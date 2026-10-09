#pragma once

// Finds moments when a set of outcome tokens, exactly one of which pays 1, can be bought for
// less than 1 in total (best asks summing below 1) or sold for more (best bids summing above
// 1, done in practice by buying every No). Each such window is tracked from the first book
// change that opens it to the one that closes it. Fees and the cost of hitting several books
// in turn are not modelled; the edge is gross.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pm/book.hpp"

namespace hft::pm {

struct ArbWindow {
    std::uint32_t group = 0;
    bool buy = true;                      // asks sum below 1, else bids above 1
    std::int64_t open_ns = 0, close_ns = 0;
    std::int32_t open_edge = 0, max_edge = 0;  // 1e-4 per set of shares
    double size_at_max = 0;               // shares available on the thinnest leg at max_edge
};

class ArbScanner {
   public:
    struct GroupStats {
        std::string name;
        std::uint64_t windows = 0, checks = 0;
        std::int32_t max_edge = 0;
        double open_seconds = 0;
    };

    std::uint32_t add_group(std::string name, std::vector<std::uint32_t> tokens) {
        const auto g = static_cast<std::uint32_t>(groups_.size());
        for (const std::uint32_t t : tokens) {
            if (t >= by_token_.size()) by_token_.resize(t + 1);
            by_token_[t].push_back(g);
        }
        groups_.push_back(Group{std::move(tokens)});
        stats_.push_back({std::move(name)});
        return g;
    }

    // Re-checks every group holding `token` after its book changed. `book(t)` returns the
    // book of token t; `on_close` receives each window as it closes.
    template <class BookOf, class OnClose>
    void on_book(std::uint32_t token, std::int64_t ns, BookOf&& book, OnClose&& on_close) {
        if (token >= by_token_.size()) return;
        for (const std::uint32_t g : by_token_[token]) check(g, ns, book, on_close);
    }

    // Closes every window still open, at `ns`.
    template <class OnClose>
    void finish(std::int64_t ns, OnClose&& on_close) {
        for (std::uint32_t g = 0; g < groups_.size(); ++g)
            for (const bool buy : {true, false}) close(g, buy, ns, on_close);
    }

    const std::vector<GroupStats>& stats() const { return stats_; }
    std::size_t open_windows() const {
        std::size_t n = 0;
        for (const auto& gr : groups_) n += gr.open[0] + gr.open[1];
        return n;
    }

   private:
    struct Group {
        std::vector<std::uint32_t> tokens;
        bool open[2] = {false, false};
        ArbWindow win[2] = {};  // [0] buy, [1] sell
    };

    template <class BookOf, class OnClose>
    void check(std::uint32_t g, std::int64_t ns, BookOf& book, OnClose& on_close) {
        Group& gr = groups_[g];
        ++stats_[g].checks;
        std::int32_t asks = 0, bids = 0;
        double ask_size = 1e300, bid_size = 1e300;
        bool all_asks = true, all_bids = true;
        for (const std::uint32_t t : gr.tokens) {
            const TokenBook& b = book(t);
            const std::int32_t a = b.best_ask(), bb = b.best_bid();
            if (a < 0) all_asks = false;
            else asks += a, ask_size = std::min(ask_size, b.size_at(false, a));
            if (bb < 0) all_bids = false;
            else bids += bb, bid_size = std::min(bid_size, b.size_at(true, bb));
        }
        update(g, true, all_asks ? TokenBook::kMaxPx - asks : 0, ask_size, ns, on_close);
        update(g, false, all_bids ? bids - TokenBook::kMaxPx : 0, bid_size, ns, on_close);
    }

    template <class OnClose>
    void update(std::uint32_t g, bool buy, std::int32_t edge, double size, std::int64_t ns, OnClose& on_close) {
        Group& gr = groups_[g];
        const int s = buy ? 0 : 1;
        if (edge <= 0) {
            close(g, buy, ns, on_close);
            return;
        }
        ArbWindow& w = gr.win[s];
        if (!gr.open[s]) {
            gr.open[s] = true;
            w = {g, buy, ns, 0, edge, edge, size};
            ++stats_[g].windows;
        } else if (edge > w.max_edge) {
            w.max_edge = edge, w.size_at_max = size;
        }
        stats_[g].max_edge = std::max(stats_[g].max_edge, edge);
    }

    template <class OnClose>
    void close(std::uint32_t g, bool buy, std::int64_t ns, OnClose& on_close) {
        Group& gr = groups_[g];
        const int s = buy ? 0 : 1;
        if (!gr.open[s]) return;
        gr.open[s] = false;
        gr.win[s].close_ns = ns;
        stats_[g].open_seconds += static_cast<double>(ns - gr.win[s].open_ns) * 1e-9;
        on_close(gr.win[s]);
    }

    std::vector<Group> groups_;
    std::vector<GroupStats> stats_;
    std::vector<std::vector<std::uint32_t>> by_token_;
};

}  // namespace hft::pm
