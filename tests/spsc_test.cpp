#include "core/spsc.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

using hft::SpscRing;

TEST(Spsc, FullAndEmptyAtCapacity) {
    SpscRing<int, 4> r;
    int v = -1;
    EXPECT_FALSE(r.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(9));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(r.try_pop(v));
}

TEST(Spsc, TransfersInOrderAcrossThreads) {
    static SpscRing<std::uint64_t, 64> r;
    constexpr std::uint64_t n = 2'000'000;
    std::thread producer([] {
        for (std::uint64_t i = 1; i <= n; ++i) r.push(i);
    });
    std::uint64_t expect = 1, sum = 0;
    for (std::uint64_t i = 1; i <= n; ++i) {
        const std::uint64_t v = r.pop();
        if (v != expect) break;
        ++expect, sum += v;
    }
    producer.join();
    EXPECT_EQ(expect, n + 1);
    EXPECT_EQ(sum, n * (n + 1) / 2);
}

#include <random>
#include <string>

using hft::SpscBytes;

namespace {

std::string payload(std::uint64_t i, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t k = 0; k < n; ++k) s[k] = static_cast<char>('a' + (i * 31 + k) % 26);
    return s;
}

}  // namespace

TEST(SpscBytes, WrapsAndKeepsPayloadsContiguous) {
    SpscBytes r(1024);
    std::mt19937 rng(3);
    std::uint64_t wrote = 0, read = 0;
    for (int round = 0; round < 20000; ++round) {
        const std::size_t n = rng() % 300;
        if (rng() % 2 && r.try_write(static_cast<std::int64_t>(wrote), wrote, 7, payload(wrote, n))) ++wrote;
        SpscBytes::Rec rec;
        if (rng() % 2 && r.try_read(rec)) {
            ASSERT_EQ(rec.ns, static_cast<std::int64_t>(read));
            ASSERT_EQ(rec.tag, 7u);
            ASSERT_EQ(rec.data, payload(read, rec.data.size()));
            r.release();
            ++read;
        }
    }
    EXPECT_GT(read, 5000u);
    EXPECT_FALSE(r.try_write(0, 0, 0, std::string(r.max_payload() + 1, 'x')));
}

TEST(SpscBytes, TransfersInOrderAcrossThreads) {
    static SpscBytes r(1 << 16);
    constexpr std::uint64_t n = 300'000;
    std::thread producer([] {
        for (std::uint64_t i = 0; i < n; ++i)
            while (!r.try_write(static_cast<std::int64_t>(i), 0, 0, payload(i, i % 700))) std::this_thread::yield();
    });
    std::uint64_t ok = 0;
    for (std::uint64_t i = 0; i < n;) {
        SpscBytes::Rec rec;
        if (!r.try_read(rec)) continue;
        ok += rec.ns == static_cast<std::int64_t>(i) && rec.data == payload(i, i % 700);
        r.release();
        ++i;
    }
    producer.join();
    EXPECT_EQ(ok, n);
}

#include <chrono>

#include "core/doorbell.hpp"

TEST(Doorbell, WakesASleepingConsumerAndTimesOut) {
    hft::Doorbell bell;
    auto t0 = std::chrono::steady_clock::now();
    bell.wait(bell.snapshot(), 20'000);  // nobody rings: returns after the timeout
    EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(15));

    static SpscRing<int, 1024> q;
    std::atomic<int> got{0};
    std::thread consumer([&] {
        int v = -1;
        while (got < 1000) {
            const std::uint32_t s = bell.snapshot();
            if (q.try_pop(v)) {
                ++got;
                continue;
            }
            bell.wait(s, 5'000'000);  // a missed wake-up would stall here for 5 s
        }
    });
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        q.push(i);
        bell.ring();
        if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    consumer.join();
    EXPECT_EQ(got, 1000);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
}
