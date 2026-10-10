// Pre-trade risk check cost at pm_live scale: 200 tokens in groups of 10, every token held.

#include <benchmark/benchmark.h>

#include "exec/oms.hpp"

using namespace hft::exec;

static void BM_RiskCheck(benchmark::State& state) {
    const auto tokens = static_cast<std::uint32_t>(state.range(0));
    Ledger lg(1'000'000 * kDollar);
    lg.ensure(tokens);
    Exposure x;
    x.ensure(tokens, tokens / 10 + 1);
    RiskLimits lim;
    lim.order_burst = lim.sustained_burst = 1'000'000'000;
    lim.order_rate_per_s = lim.sustained_rate_per_s = 1'000'000'000;
    Risk risk(lim);
    risk.ensure(tokens);
    for (std::uint32_t t = 0; t < tokens; ++t) {
        risk.set_group(t, t / 10), x.set_group(t, t / 10), lg.set_group(t, t / 10);
        lg.fill(t + 1, t, Side::Buy, 4000, 10 * kShare, 0);
        lg.settled(t + 1);
    }
    hft::pm::MarketRules rules;
    rules.tick = 10;
    const MarketView mv{4000, 4010, true, false, &rules};
    OrderIntent o{5, Side::Buy, Tif::Gtc, false, 0, 4000, 10 * kShare, 0};
    Ns now = 1;
    for (auto _ : state) {
        o.token = static_cast<std::uint32_t>(now % tokens);
        benchmark::DoNotOptimize(risk.check(o, mv, x, lg, now++));
    }
}
BENCHMARK(BM_RiskCheck)->Arg(2)->Arg(40)->Arg(200);

// One order life cycle: submit (risk check, reservations, New), ack, partial fill (ledger),
// cancel, cancel ack. Five order-manager operations per iteration.
static void BM_OmsCycle(benchmark::State& state) {
    constexpr Ns kSec = 1'000'000'000;
    Ledger ledger(1'000'000 * kDollar);
    Exposure exp;
    RiskLimits lim;
    lim.order_burst = lim.sustained_burst = lim.cancel_burst = 1'000'000'000;
    lim.order_rate_per_s = lim.sustained_rate_per_s = lim.cancel_rate_per_s = 1'000'000'000;
    Risk risk(lim);
    OmsConfig cfg;
    cfg.keep_done = kSec;
    Oms oms(1, risk, exp, ledger, cfg);
    ledger.ensure(200), exp.ensure(200, 20), risk.ensure(200), oms.ensure(200);
    for (std::uint32_t t = 0; t < 200; ++t) ledger.set_group(t, t / 10), exp.set_group(t, t / 10), risk.set_group(t, t / 10);
    hft::pm::MarketRules rules;
    rules.tick = 10;
    const MarketView mv{4000, 4100, true, false, &rules};
    std::uint64_t sent = 0;
    const auto sink = [&](const VenueReq& r) { sent += r.cl_id; };
    const auto on = [](const Order&, OrdEvent, Qty, Px) {};
    Ns now = kSec;
    std::uint64_t fill = 1;
    std::uint32_t tok = 0;
    for (auto _ : state) {
        now += 1000;
        tok = tok + 1 == 200 ? 0 : tok + 1;
        const std::uint64_t cl = oms.submit({tok, Side::Buy, Tif::Gtc, false, 0, 4000, 10 * kShare, 0}, mv, now, sink);
        if (!cl) {
            state.SkipWithError("submit refused");
            break;
        }
        oms.on_report({VenueRpt::Ack, VenueRpt::Live, 0, 0, 0, 0, cl, 1, 0, now}, now, on);
        oms.on_report({VenueRpt::Fill, VenueRpt::None, 0, 4000, 5 * kShare, 0, cl, 1, fill, now}, now, on);
        oms.on_report({VenueRpt::Settled, VenueRpt::None, 0, 0, 0, 0, cl, 1, fill++, now}, now, on);
        oms.cancel(cl, now, sink);
        oms.on_report({VenueRpt::CancelAck, VenueRpt::None, 0, 0, 0, 0, cl, 1, 0, now}, now, on);
        if ((fill & 1023) == 0) {
            state.PauseTiming();
            oms.on_timer(now + 2 * kSec, sink);  // free finished orders outside the timed region
            for (std::uint32_t t = 0; t < 200; ++t) ledger.resolve(t, true);  // wins, so cash never runs out
            state.ResumeTiming();
        }
    }
    benchmark::DoNotOptimize(sent);
    state.counters["killed"] = risk.killed() ? static_cast<double>(risk.kill_reason()) : 0;
    state.counters["illegal"] = static_cast<double>(oms.illegal_count());
    state.counters["rejects"] = static_cast<double>(risk.count(Reject::Killed) + risk.count(Reject::Position) + risk.count(Reject::Group) + risk.count(Reject::Gross) + risk.count(Reject::TooMany) + risk.count(Reject::Throttle) + risk.count(Reject::Cash));
    state.counters["ns_per_op"] = benchmark::Counter(static_cast<double>(state.iterations()) * 5,
                                                     benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_OmsCycle);
