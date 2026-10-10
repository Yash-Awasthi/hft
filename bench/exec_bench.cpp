// Pre-trade risk check cost at pm_live scale: 200 tokens in groups of 10, every token held.

#include <benchmark/benchmark.h>

#include "exec/risk.hpp"

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
