#pragma once

// Builds single ITCH 5.0 messages for tests, one symbol (locate 1, "TEST").

#include <cstdint>
#include <vector>

struct ItchWriter {
    std::vector<std::uint8_t> b;
    std::uint64_t ts = 34'200'000'000'000;  // 09:30

    ItchWriter& head(char t) {
        b.clear();
        b.push_back(static_cast<std::uint8_t>(t));
        return n(1, 2).n(0, 2).n(ts, 6);
    }
    ItchWriter& n(std::uint64_t v, int bytes) {
        for (int s = (bytes - 1) * 8; s >= 0; s -= 8)
            b.push_back(static_cast<std::uint8_t>(v >> s));
        return *this;
    }
    ItchWriter& c(char v) { return n(static_cast<std::uint8_t>(v), 1); }
    ItchWriter& stock() { return n(0x5445535420202020ull, 8); }

    ItchWriter& add(std::uint64_t ref, char side, std::uint32_t qty, std::uint32_t px) {
        return head('A').n(ref, 8).c(side).n(qty, 4).stock().n(px, 4);
    }
    ItchWriter& exec(std::uint64_t ref, std::uint32_t qty) {
        return head('E').n(ref, 8).n(qty, 4).n(9, 8);
    }
    ItchWriter& cancel(std::uint64_t ref, std::uint32_t qty) {
        return head('X').n(ref, 8).n(qty, 4);
    }
    ItchWriter& del(std::uint64_t ref) { return head('D').n(ref, 8); }
    ItchWriter& hidden(char side, std::uint32_t qty, std::uint32_t px) {
        return head('P').n(0, 8).c(side).n(qty, 4).stock().n(px, 4).n(9, 8);
    }
};
