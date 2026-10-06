#include "data/store.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "temp_dir.hpp"

using namespace hft::data;

namespace {

// Writes 3 interleaved symbols and checks each reads back complete and in order.
void roundtrip(std::size_t budget, std::uint32_t chunk_msgs) {
    TempDir dir("store");
    std::map<std::uint16_t, std::vector<std::pair<std::uint64_t, std::string>>> want;
    {
        StoreWriter w(dir.path, budget, chunk_msgs);
        for (std::uint64_t seq = 0; seq < 100; ++seq) {
            const auto locate = static_cast<std::uint16_t>(seq % 3 == 0   ? 7
                                                           : seq % 3 == 1 ? 8
                                                                          : 40000);
            const std::string msg = "msg-" + std::to_string(seq) + std::string(seq % 5, 'x');
            w.append(locate, seq, seq * 1000, reinterpret_cast<const std::uint8_t*>(msg.data()),
                     msg.size());
            want[locate].emplace_back(seq, msg);
        }
        w.finish();
        EXPECT_GT(w.compressed_bytes(), 0u);
    }
    for (const auto& [locate, records] : want) {
        SymbolReader r(dir.path, locate);
        Record rec{};
        for (const auto& [seq, msg] : records) {
            ASSERT_TRUE(r.next(rec));
            EXPECT_EQ(rec.seq, seq);
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(rec.data), rec.len), msg);
        }
        EXPECT_FALSE(r.next(rec));
    }
}

}  // namespace

TEST(Store, RoundtripSingleChunk) { roundtrip(1 << 20, 1000); }

TEST(Store, RoundtripSmallChunks) { roundtrip(1 << 20, 4); }

TEST(Store, RoundtripTinyBudgetForcesFlushes) { roundtrip(64, 1000); }

TEST(Store, IndexRecordsChunkBounds) {
    TempDir dir("store");
    {
        StoreWriter w(dir.path, 1 << 20, 2);
        const std::uint8_t m[1] = {'D'};
        for (std::uint64_t seq = 10; seq < 15; ++seq) w.append(5, seq, seq * 100, m, 1);
    }
    std::ifstream idx(dir.path / "00005.idx", std::ios::binary);
    IndexEntry e{};
    std::vector<IndexEntry> all;
    while (idx.read(reinterpret_cast<char*>(&e), sizeof e)) all.push_back(e);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].first_seq, 10u);
    EXPECT_EQ(all[0].first_ts, 1000u);
    EXPECT_EQ(all[0].n_msgs, 2u);
    EXPECT_EQ(all[1].first_seq, 12u);
    EXPECT_EQ(all[2].n_msgs, 1u);
    EXPECT_EQ(all[1].offset, all[0].comp_size);
}

TEST(Store, MissingSymbolThrows) {
    TempDir dir("store");
    EXPECT_THROW(SymbolReader(dir.path, 9), std::runtime_error);
}

TEST(Store, SeekStartsAtChunk) {
    TempDir dir("store");
    {
        StoreWriter w(dir.path, 1 << 20, 2);
        const std::uint8_t m[1] = {'D'};
        for (std::uint64_t seq = 10; seq < 15; ++seq) w.append(5, seq, seq * 100, m, 1);
    }
    SymbolReader r(dir.path, 5);
    ASSERT_EQ(r.index().size(), 3u);
    Record rec{};
    r.seek(1);
    ASSERT_TRUE(r.next(rec));
    EXPECT_EQ(rec.seq, 12u);
    r.seek(2);
    ASSERT_TRUE(r.next(rec));
    EXPECT_EQ(rec.seq, 14u);
    EXPECT_FALSE(r.next(rec));
    r.seek(0);
    ASSERT_TRUE(r.next(rec));
    EXPECT_EQ(rec.seq, 10u);
}

TEST(Store, MergedReaderRestoresFeedOrder) {
    TempDir dir("merged");
    const std::uint16_t locs[4] = {3, 9, 40000, 11};
    {
        StoreWriter w(dir.path, 1 << 20, 3);
        for (std::uint64_t seq = 1; seq <= 500; ++seq) {
            const std::uint16_t loc = locs[(seq * 7 + seq / 13) % 4];
            const std::uint8_t m[3] = {static_cast<std::uint8_t>(seq),
                                       static_cast<std::uint8_t>(loc), 'x'};
            w.append(loc, seq, seq * 10, m, 1 + seq % 3);
        }
    }
    for (std::size_t ahead : {1, 4, 64}) {
        MergedReader r(dir.path, {3, 9, 11}, ahead);
        Record rec{};
        std::uint16_t loc = 0;
        std::uint64_t last = 0, n = 0;
        while (r.next(rec, loc)) {
            EXPECT_GT(rec.seq, last);
            EXPECT_NE(loc, 40000);
            EXPECT_EQ(rec.len, 1 + rec.seq % 3);
            EXPECT_EQ(rec.data[0], static_cast<std::uint8_t>(rec.seq));
            if (rec.len > 1) {
                EXPECT_EQ(rec.data[1], static_cast<std::uint8_t>(loc));
            }
            last = rec.seq;
            ++n;
        }
        std::uint64_t want = 0;
        for (std::uint64_t seq = 1; seq <= 500; ++seq)
            want += locs[(seq * 7 + seq / 13) % 4] != 40000;
        EXPECT_EQ(n, want);
    }
    EXPECT_THROW(MergedReader(dir.path, {5}), std::runtime_error);
    MergedReader none(dir.path, {});
    Record rec{};
    std::uint16_t loc = 0;
    EXPECT_FALSE(none.next(rec, loc));
}
