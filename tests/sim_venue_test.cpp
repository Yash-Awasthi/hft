#include "exec/sim_venue.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace hft::exec;
using hft::pm::TokenBook;

namespace {

constexpr Ns kMs = 1'000'000;

struct Rig {
    std::vector<TokenBook> books = std::vector<TokenBook>(2);
    struct BookOf {
        std::vector<TokenBook>* b;
        const TokenBook& operator()(std::uint32_t t) const { return (*b)[t]; }
    };
    SimVenue<BookOf> v;
    std::vector<VenueRpt> got;
    std::uint64_t next_cl = (std::uint64_t{1} << 48) | 1;

    explicit Rig(SimConfig c = {}) : v(c, BookOf{&books}) {
        v.ensure(2);
        hft::pm::MarketRules r;
        r.tick = 10;
        r.fee_rate_ppm = 0;  // fees checked in their own test
        v.set_rules(0, r), v.set_rules(1, r);
        // Token 0: bids 0.40 x 100, 0.39 x 50; asks 0.41 x 30, 0.42 x 40, 0.44 x 100.
        books[0].set(true, 4000, 100), books[0].set(true, 3900, 50);
        books[0].set(false, 4100, 30), books[0].set(false, 4200, 40), books[0].set(false, 4400, 100);
    }
    std::uint64_t send(Side s, Px px, double shares, Tif tif = Tif::Gtc, Ns now = 0, bool post_only = false) {
        const std::uint64_t cl = next_cl++;
        v.request({VenueReq::New, s, tif, post_only, 0, px, static_cast<Qty>(shares * kShare), cl}, now);
        return cl;
    }
    void ask(VenueReq::Kind k, std::uint64_t cl, Ns now = 0) { v.request({k, Side::Buy, Tif::Gtc, false, 0, 0, 0, cl}, now); }
    void run(Ns t) {
        v.run(t, true, [&](const VenueRpt& r) { got.push_back(r); });
    }
    std::vector<VenueRpt> of(std::uint64_t cl, VenueRpt::Kind k) const {
        std::vector<VenueRpt> out;
        for (const auto& r : got)
            if (r.cl_id == cl && r.kind == k) out.push_back(r);
        return out;
    }
    Qty filled(std::uint64_t cl) const {
        Qty n = 0;
        for (const auto& r : of(cl, VenueRpt::Fill)) n += r.qty;
        return n;
    }
};

}  // namespace

TEST(SimVenue, TakerWalksLevelsWithinItsLimitAndTheRestExpires) {
    Rig g;
    const auto cl = g.send(Side::Buy, 4200, 100, Tif::Fak);
    g.run(10 * kMs);
    const auto f = g.of(cl, VenueRpt::Fill);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].px, 4100);
    EXPECT_EQ(f[0].qty, 30 * kShare);
    EXPECT_EQ(f[1].px, 4200);  // 0.44 is beyond the limit
    EXPECT_EQ(f[1].qty, 40 * kShare);
    EXPECT_EQ(g.of(cl, VenueRpt::Ack)[0].status, VenueRpt::Matched);
    EXPECT_EQ(g.of(cl, VenueRpt::Expired).size(), 1u);
    EXPECT_TRUE(g.v.check());
}

TEST(SimVenue, TakenSharesAreNotTakenTwiceUntilTheLevelUpdates) {
    Rig g;
    const auto a = g.send(Side::Buy, 4100, 20, Tif::Fak), b = g.send(Side::Buy, 4100, 20, Tif::Fak);
    g.run(1);
    EXPECT_EQ(g.filled(a), 20 * kShare);
    EXPECT_EQ(g.filled(b), 10 * kShare);  // only 30 displayed
    g.books[0].set(false, 4100, 30);
    g.v.on_level(0, false, 4100);  // the exchange shows 30 again: our taking is reflected there
    const auto c = g.send(Side::Buy, 4100, 25, Tif::Fak);
    g.run(2);
    EXPECT_EQ(g.filled(c), 25 * kShare);
}

TEST(SimVenue, FillOrKillIsAllOrNothing) {
    Rig g;
    const auto no = g.send(Side::Buy, 4200, 71, Tif::Fok);  // 70 available within the limit
    const auto yes = g.send(Side::Buy, 4200, 70, Tif::Fok);
    g.run(1);
    EXPECT_EQ(g.filled(no), 0);
    EXPECT_EQ(g.of(no, VenueRpt::Expired).size(), 1u);
    EXPECT_EQ(g.filled(yes), 70 * kShare);
    EXPECT_TRUE(g.of(yes, VenueRpt::Expired).empty());
}

TEST(SimVenue, RestingOrderWaitsBehindTheQueueThenFills) {
    Rig g;
    const auto cl = g.send(Side::Buy, 4000, 10);  // joins 100 shares at 0.40
    g.run(1);
    EXPECT_EQ(g.of(cl, VenueRpt::Ack)[0].status, VenueRpt::Live);
    g.v.on_trade(0, 4000, 60, false, 2);  // 60 of the 100 ahead traded
    g.run(2);
    EXPECT_EQ(g.filled(cl), 0);
    g.books[0].set(true, 4000, 20);  // more cancelled: only 20 shown, so at most 20 ahead
    g.v.on_level(0, true, 4000);
    g.v.on_trade(0, 4000, 25, false, 3);  // 20 ahead, then 5 to us
    g.run(3);
    EXPECT_EQ(g.filled(cl), 5 * kShare);
    g.v.on_trade(0, 3990, 1, false, 4);  // a trade below our bid: it went through us
    g.run(4);
    EXPECT_EQ(g.filled(cl), 10 * kShare);
    for (const auto& f : g.of(cl, VenueRpt::Fill)) {  // our price, no maker fee
        EXPECT_EQ(f.px, 4000);
        EXPECT_EQ(f.fee, 0);
    }
    EXPECT_TRUE(g.v.check());
}

TEST(SimVenue, AnImprovingOrderHasNobodyAhead) {
    Rig g;
    const auto cl = g.send(Side::Buy, 4050, 10);
    g.run(1);
    g.v.on_trade(0, 4050, 4, false, 2);
    g.run(2);
    EXPECT_EQ(g.filled(cl), 4 * kShare);
}

TEST(SimVenue, RuleRejects) {
    Rig g;
    const auto off_tick = g.send(Side::Buy, 4005, 10), small = g.send(Side::Buy, 4000, 4.99),
               odd = g.send(Side::Buy, 4000, 10.005), cross = g.send(Side::Buy, 4100, 10, Tif::Gtc, 0, true);
    g.run(1);
    EXPECT_EQ(g.of(off_tick, VenueRpt::Reject)[0].reason, g.v.kTick);
    EXPECT_EQ(g.of(small, VenueRpt::Reject)[0].reason, g.v.kSize);
    EXPECT_EQ(g.of(odd, VenueRpt::Reject)[0].reason, g.v.kSize);
    EXPECT_EQ(g.of(cross, VenueRpt::Reject)[0].reason, g.v.kPostOnlyCross);
    const auto resting = g.send(Side::Buy, 3900, 10, Tif::Gtc, 2);
    g.run(2);
    g.v.close(0, 3);
    g.run(3);
    EXPECT_EQ(g.of(resting, VenueRpt::CancelAck).size(), 1u);
    const auto late = g.send(Side::Buy, 3900, 10, Tif::Gtc, 4);
    g.run(4);
    EXPECT_EQ(g.of(late, VenueRpt::Reject)[0].reason, g.v.kClosed);
}

TEST(SimVenue, TakerFeesFollowTheMarketSchedule) {
    Rig g;
    hft::pm::MarketRules r;
    r.tick = 10;
    r.fee_rate_ppm = 50'000;
    g.v.set_rules(0, r);
    const auto cl = g.send(Side::Buy, 4100, 30, Tif::Fak);
    g.run(1);
    EXPECT_EQ(g.of(cl, VenueRpt::Fill)[0].fee, taker_fee(30 * kShare, 4100, r));
}

TEST(SimVenue, SportsDelayHoldsMarketableOrdersThenMatches) {
    Rig g;
    hft::pm::MarketRules r;
    r.tick = 10, r.fee_rate_ppm = 0, r.delay_ms = 1000;
    g.v.set_rules(0, r);
    const auto a = g.send(Side::Buy, 4100, 10, Tif::Fak, 0), b = g.send(Side::Buy, 4100, 10, Tif::Fak, 0);
    g.run(1);
    EXPECT_EQ(g.of(a, VenueRpt::Ack)[0].status, VenueRpt::Delayed);
    EXPECT_EQ(g.filled(a), 0);
    g.ask(VenueReq::Cancel, b, 500 * kMs);
    g.run(999 * kMs);
    EXPECT_EQ(g.filled(a), 0);
    g.run(1000 * kMs);
    EXPECT_EQ(g.filled(a), 10 * kShare);
    EXPECT_EQ(g.of(b, VenueRpt::CancelAck).size(), 1u);
    EXPECT_EQ(g.filled(b), 0);
}

TEST(SimVenue, CancelAnswers) {
    Rig g;
    const auto rest = g.send(Side::Buy, 3900, 10), done = g.send(Side::Buy, 4100, 10, Tif::Fak);
    g.run(1);
    g.ask(VenueReq::Cancel, rest, 2), g.ask(VenueReq::Cancel, done, 2), g.ask(VenueReq::Cancel, 999, 2);
    g.run(2);
    EXPECT_EQ(g.of(rest, VenueRpt::CancelAck).size(), 1u);
    EXPECT_EQ(g.of(done, VenueRpt::CancelReject).size(), 1u);
    EXPECT_EQ(g.of(999, VenueRpt::CancelReject)[0].reason, g.v.kNotFound);
}

TEST(SimVenue, StatusResendsFillsThenTheState) {
    Rig g;
    const auto part = g.send(Side::Buy, 4000, 10);
    g.run(1);
    g.v.on_trade(0, 3990, 1, false, 2);  // fills it
    const auto open = g.send(Side::Buy, 3900, 10, Tif::Gtc, 3);
    g.run(3);
    g.got.clear();
    g.ask(VenueReq::Status, part, 4), g.ask(VenueReq::Status, open, 4), g.ask(VenueReq::Status, 12345, 4);
    g.run(4);
    ASSERT_EQ(g.of(part, VenueRpt::Fill).size(), 1u);
    EXPECT_EQ(g.of(part, VenueRpt::Status)[0].status, VenueRpt::Filled);
    EXPECT_EQ(g.of(part, VenueRpt::Status)[0].qty, 10 * kShare);
    EXPECT_EQ(g.of(open, VenueRpt::Status)[0].status, VenueRpt::Live);
    EXPECT_EQ(g.of(12345, VenueRpt::Status)[0].status, VenueRpt::NotFound);
}

TEST(SimVenue, LatencyAndOrderedDelivery) {
    SimConfig c;
    c.lat_in = 40 * kMs, c.lat_out = 50 * kMs, c.jitter = 30 * kMs, c.seed = 3;
    Rig g(c);
    std::vector<std::uint64_t> cls;
    for (int i = 0; i < 20; ++i) cls.push_back(g.send(Side::Buy, 3900, 10, Tif::Gtc, i * kMs));
    g.run(89 * kMs);
    EXPECT_TRUE(g.got.empty());  // nothing can come back before 40 + 50 ms
    g.run(1000 * kMs);
    ASSERT_EQ(g.got.size(), 20u);
    // One connection: acks come back in the order the venue sent them, whatever the jitter.
    for (std::size_t i = 1; i < g.got.size(); ++i) EXPECT_GE(g.got[i].venue_ns, g.got[i - 1].venue_ns);
}

TEST(SimVenue, RequestsArriveInTheOrderTheyWereSent) {
    SimConfig c;
    c.lat_in = 50 * kMs, c.jitter = 40 * kMs;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        c.seed = seed;
        Rig g(c);
        const auto cl = g.send(Side::Buy, 3900, 10, Tif::Gtc, 0);
        g.ask(VenueReq::Cancel, cl, 1);  // sent 1 ns later: must not overtake the order
        g.run(1000 * kMs);
        ASSERT_EQ(g.of(cl, VenueRpt::CancelAck).size(), 1u) << seed;
        ASSERT_TRUE(g.of(cl, VenueRpt::CancelReject).empty()) << seed;
    }
}

TEST(SimVenue, SettlementArrivesLaterAndDoesNotHoldOtherReports) {
    SimConfig c;
    c.settle_delay = 2000 * kMs;
    Rig g(c);
    const auto a = g.send(Side::Buy, 4100, 10, Tif::Fak, 0);
    const auto b = g.send(Side::Buy, 3900, 10, Tif::Gtc, 10 * kMs);
    g.run(20 * kMs);
    EXPECT_EQ(g.of(b, VenueRpt::Ack).size(), 1u);  // not stuck behind a's settlement
    EXPECT_TRUE(g.of(a, VenueRpt::Settled).empty());
    g.run(2000 * kMs);
    EXPECT_EQ(g.of(a, VenueRpt::Settled).size(), 1u);

    SimConfig f;
    f.p_settle_fail_ppm = 1'000'000;
    Rig h(f);
    const auto x = h.send(Side::Buy, 4100, 10, Tif::Fak);
    h.run(3000 * kMs);
    EXPECT_EQ(h.of(x, VenueRpt::SettleFailed).size(), 1u);
}

TEST(SimVenue, DroppedFillIsStillOnTheVenueAndShowsInStatus) {
    SimConfig c;
    c.p_drop_fill_ppm = 1'000'000;
    Rig g(c);
    const auto cl = g.send(Side::Buy, 4100, 10, Tif::Fak);
    g.run(1);
    EXPECT_EQ(g.filled(cl), 0);  // the fill happened; its report was lost
    EXPECT_EQ(g.v.stats().dropped, 1u);
    g.ask(VenueReq::Status, cl, 2);
    g.run(2);
    EXPECT_EQ(g.of(cl, VenueRpt::Status)[0].qty, 10 * kShare);  // the order manager sees the gap
}

TEST(SimVenue, DroppedAckDuplicatesAndDisconnects) {
    {
        SimConfig c;
        c.p_drop_ack_ppm = 1'000'000;
        Rig g(c);
        const auto cl = g.send(Side::Buy, 4100, 10, Tif::Fak);
        g.run(1);
        EXPECT_TRUE(g.of(cl, VenueRpt::Ack).empty());
        EXPECT_EQ(g.filled(cl), 10 * kShare);
    }
    {
        SimConfig c;
        c.p_dup_ppm = 1'000'000;
        Rig g(c);
        const auto cl = g.send(Side::Buy, 4100, 10, Tif::Fak);
        g.run(1);
        EXPECT_EQ(g.of(cl, VenueRpt::Ack).size(), 2u);
        ASSERT_EQ(g.of(cl, VenueRpt::Fill).size(), 2u);
        EXPECT_EQ(g.of(cl, VenueRpt::Fill)[0].fill_id, g.of(cl, VenueRpt::Fill)[1].fill_id);  // same fill twice
    }
    {
        SimConfig c;
        c.disc_every = 10'000 * kMs, c.disc_for = 3000 * kMs;  // offline for the first 3 s of every 10 s
        Rig g(c);
        const auto cl = g.send(Side::Buy, 3900, 10, Tif::Gtc, 1000 * kMs);
        g.run(2999 * kMs);
        EXPECT_TRUE(g.of(cl, VenueRpt::Ack).empty());  // held
        g.run(3000 * kMs);
        EXPECT_EQ(g.of(cl, VenueRpt::Ack).size(), 1u);
        EXPECT_GE(g.v.stats().held, 1u);
    }
}

TEST(SimVenue, CompatModeFillsOnlyTheSideTheFeedNames) {
    SimConfig c;
    c.compat_side = true;
    Rig g(c);
    const auto bid = g.send(Side::Buy, 4050, 10);
    g.run(1);
    g.v.on_trade(0, 4040, 5, true, 2);  // below our bid, but the feed says the taker bought
    g.run(2);
    EXPECT_EQ(g.filled(bid), 0);
    g.v.on_trade(0, 4040, 5, false, 3);
    g.run(3);
    EXPECT_EQ(g.filled(bid), 10 * kShare);

    Rig p;  // production rule: price alone decides
    const auto b2 = p.send(Side::Buy, 4050, 10);
    p.run(1);
    p.v.on_trade(0, 4040, 5, true, 2);
    p.run(2);
    EXPECT_EQ(p.filled(b2), 10 * kShare);
}

TEST(SimVenue, SameSeedSameRunDifferentSeedDifferentFaults) {
    auto run = [](std::uint64_t seed) {
        SimConfig c;
        c.seed = seed, c.jitter = 20 * kMs, c.p_drop_fill_ppm = 300'000, c.p_dup_ppm = 300'000;
        Rig g(c);
        for (int i = 0; i < 50; ++i) {
            g.send(Side::Buy, 4100, 1, Tif::Fak, i * kMs);
            g.books[0].set(false, 4100, 30), g.v.on_level(0, false, 4100);
        }
        g.run(10'000 * kMs);
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& r : g.got) h = (h ^ (r.cl_id * 31 + r.kind * 7 + static_cast<std::uint64_t>(r.qty))) * 1099511628211ull;
        return h;
    };
    EXPECT_EQ(run(1), run(1));
    EXPECT_NE(run(1), run(2));
}
