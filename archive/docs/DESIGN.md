# L3 Market-Making Lab

A complete HFT market-making system in C++, run on real Nasdaq order-level data, and
the research built on it.

- **Strategy**: signals from book state, queue dynamics, trade flow and cross-asset
  lead-lag; fills simulated at the exact position in the real queue; quoting by optimal
  control solved on a data-estimated model; PnL decomposed into spread capture, adverse
  selection, fees and inventory.
- **Research**: a pre-registered forecast of how half-penny ticks and the 10 mil fee cap
  will change market-making economics before any post-change data exists; market
  impact measured from public data and split into mechanical and reactive parts; a
  metamorphic test of whether 2026 generative market simulators respond to
  interventions the way real markets do.
- **Engineering**: zero-allocation ITCH decoder, L3 order book and deterministic
  matching engine with `memcpy` forking, hand-written SIMD inference kernel running a
  transformer inside the trading loop, and verification down to model-checked
  concurrency and mutation-tested suites. A live pipeline (MoldUDP64 feed handler with
  receive paths up to AF_XDP and DPDK, OUCH order entry, measured tick-to-trade
  latency) is a stretch goal.

Scope is software only. Kernel bypass is in scope as code (AF_XDP, DPDK with virtual
drivers); FPGA and physical NIC tuning are out of scope. Everything is built and
measured under WSL2 on one laptop; no native Linux is available.

## Why now

- **Market structure change with no data yet.** Half-penny ticks for ~1,800 stocks and
  the 30 to 10 mil access fee cap (Rules 612 / 610) are adopted, with compliance
  postponed to November 2027. In June 2026 the SEC proposed rescinding the trade-through
  rule (611) and the locked/crossed ban (610(e)). Queue and rebate economics for market
  makers will change, and no post-change data exists to measure it.
- **Small learned models fit inside the trading loop.** Event-level transformers with
  ~7k to 14k parameters (Schaurecker et al., Oct 2026) beat larger LOB forecasters at
  sub-millisecond inference on one CPU thread. A hand-written C++ inference kernel can
  push that into the microsecond range, fast enough to quote on.
- **Counterfactual simulation is the current frontier.** M3 (Jul 2026) generates order
  flow with a Transformer and executes it through deterministic matching rules. Linna
  et al. (Sep 2026) inject hypothetical messages into a pretrained forecaster to
  estimate impact. Noble, Rosenbaum and Souilmi (Mar 2026) reach realistic execution
  behaviour with a power-law trade-flow feedback kernel. This project needs exactly
  such a simulator for its forecast, and tests these models as an extension.

## Questions

**Q1. Edge decomposition.** For a market maker whose orders sit at their exact position
in the real queue, how does PnL split into spread capture, adverse selection, rebates
and inventory, and how much does each of queue priority, a learned short-horizon
signal and latency contribute?

**Q2. Regime forecast.** Under half-penny ticks and a 10 mil fee cap, how do spread,
queue length, fill probability, markouts and market-maker PnL per share change? Frozen
and published before November 2027. Permitted locked/crossed quotes are studied as a
separate simulator scenario (section 5).

Extensions, after Q1 and Q2:
- **E1. Counterfactual validity.** Do learned simulators obey structural market laws
  under intervention (metamorphic testing, section 7)?
- **E2. Impact law.** How large is impact measured from public data, where does the
  exponent in `ΔP(Q) ~ Q^ψ` depart from 0.5, and how much of impact is mechanical versus
  a reaction by other participants (section 6)?

## Data

- Nasdaq TotalView-ITCH 5.0 sample days (`emi.nasdaq.com/ITCH/`), every order with its
  ID. About 20 days are available:
  - Dec 8 to 12, 2025: five consecutive days, 8 to 12.5 GB compressed each
  - May 15 and 18, 2026; Jun 12, 2026: 13 to 18 GB compressed each
  - Nov 28, 2025, a half-day session after Thanksgiving, 5 GB
  - Seven days from 2019, one from 2020 and two from 2021
- Split: train Dec 8 to 10, validation Dec 11, test Dec 12 plus May 15, May 18 and
  Jun 12, 2026, all locked and run once. The 2019 to 2021 days and the Nov 28 half-day
  test robustness across regimes and are never used for fitting.
- The unit of evaluation is the stock-day (about 50 stocks per day). Stock-days within
  one day share market-wide shocks, so the test set's effective sample size lies
  between four days and about 200 stock-days; intervals are clustered by day.
- The small number of days limits statistical power; confidence intervals are
  reported throughout and claims sized to them.
- Only the days in use are ingested (about 150 GB in the store, close to the size of
  the gzip downloads), not the whole archive.
- Universe: about 50 liquid stocks, half large-tick (spread usually one tick), half
  small-tick, plus SPY, QQQ and QQQ constituents for cross-asset signals. The tick
  change only affects the large-tick group, giving a natural control for it. The fee
  cap applies to every stock priced at $1 or more, so it has no control group.

Order IDs are the key: a simulated order can join the real queue at a known position
and fills only after the real orders ahead of it trade or cancel.

## Notation

| Symbol | Meaning |
|--------|---------|
| `b_t, a_t` | Best bid and ask |
| `s_t = a_t - b_t` | Spread |
| `m_t = (a_t + b_t)/2` | Mid price |
| `V^b_t, V^a_t` | Size at best bid and ask |
| `I_t` | Queue imbalance at best |
| `ε_t` | Trade sign, +1 buyer-initiated, -1 seller-initiated |
| `θ` | Side of one of our fills, +1 buy, -1 sell |
| `q_t` | Strategy inventory |
| `T` | End of the trading day |
| `δ` | Tick size |
| `ψ` | Impact exponent |
| `σ` | Volatility of the mid |
| `γ` | Inventory risk aversion |
| `Q, V_D` | Metaorder size, daily volume |
| `ΔP(Q)` | Price impact of a metaorder of size `Q` |
| `h, τ` | Forecast horizon, markout horizon |
| `d` | Quote distance from the reservation price |
| `κ` | Decay exponent of trade-sign autocorrelation |

---

# Part I: Quant

## 1. Signals

Targets `y_t(h) = m_{t+h} - m_t` in ticks at several horizons, `h` from 10 events to
1 s, in both event time and clock time.

ITCH covers Nasdaq only, about 15 to 20% of US volume, so every signal sees a partial
book. Signal strength is reported with that caveat, and how much predictability
survives on one venue's view is itself a result.

### Book state

| Signal | Definition |
|--------|-----------|
| Queue imbalance | `I_t = (V^b - V^a) / (V^b + V^a)` |
| Microprice | `m_t + s_t * g(I_t, s_t)`, `g` from a Markov chain on (imbalance bucket, spread), Stoikov (2018) |
| Multi-level OFI | Signed depth changes at levels 1..k (Cont, Kukanov, Stoikov 2014; Xu, Gould, Howison 2018), combined by ridge |

### Queue dynamics

| Signal | Definition |
|--------|-----------|
| Queue race | Probability the bid queue depletes before the ask queue, from per-side depletion rates (Cont, de Larrard 2013) |
| Quote fragility | Cancel rate at best, add/cancel ratio, time since last spread change |

### Trade flow

| Signal | Definition |
|--------|-----------|
| Hawkes trade intensity | `λ_t = μ + Σ α e^{-β(t - t_i)}` per side, MLE fit, signal `λ^buy - λ^sell` |
| Trade-sign memory | Long-memory autocorrelation of aggressor signs, aggressor imbalance, trade-size buckets; flags metaorders being worked |
| Hidden liquidity | Executions against non-displayed orders (ITCH `P` messages) |

### Cross-asset

| Signal | Definition |
|--------|-----------|
| Index lead-lag | SPY and QQQ order flow and mid changes predicting single-stock moves |
| Sector lead-lag | Lagged flow of correlated stocks, lag structure estimated by cross-correlation at event resolution |
| ETF basis | QQQ mid minus the weighted mids of its constituents |

### Auction and regime

| Signal | Definition |
|--------|-----------|
| Auction imbalance | Net order imbalance messages (ITCH `I`) before the opening and closing crosses |
| Regime | Realized volatility in ticks, spread state, time of day; used to condition other signals |

### Learned

| Signal | Definition |
|--------|-----------|
| Event transformer | ~10k-parameter model on the last N events, trained in PyTorch, served by the C++ kernel (section 11) |

### Combination

- Ridge regression baseline.
- LightGBM for nonlinear interactions.
- Online recursive least squares with exponential forgetting, adapting within the day.

### Evaluation

- Spearman IC, out-of-sample R², IC decay against `h`, large-tick versus small-tick split.
- **Tradable IC**: IC restricted to predictions whose magnitude exceeds half-spread
  plus fees, since a correct prediction smaller than the cost cannot be traded.
- Purged walk-forward validation with an embargo between train and test windows.
- Leakage check: every feature at time `t` must be bit-identical after the events
  after `t` are perturbed (deleted, shuffled or replaced with synthetic flow). A
  one-event delay test is not used: at event resolution the latest event legitimately
  carries most of the signal, and a feature that peeks one event ahead survives it.
- Number of variants tried is recorded and the final Sharpe reported as a deflated
  Sharpe ratio, computed on intraday PnL buckets with day-clustered errors, since four
  test days give too few daily observations.

### Implementation

- Every feature updates in O(1) per event in C++ using exponential moving averages and
  ring buffers.
- Research and trading call the same C++ feature code (through nanobind for research),
  so training and live features cannot diverge.
- A test asserts that features computed in batch and in streaming mode are identical.

## 2. Fills and adverse selection

### Queue position

A virtual order joins the back of the real queue. Every ITCH cancel and execution
carries an order ID, so whether it was ahead of the virtual order is known exactly;
queue position is tracked, not estimated.

### Simulation limits and their handling

| Limit | Handling |
|-------|----------|
| Reaction to our quote | Real participants might respond to our added size. Size kept small relative to the queue; sensitivity measured by rerunning in the queue-reactive simulator, where flow responds to the book |
| Book divergence | A fill for us means a real order behind us should not have filled. Our fills consume the liquidity of the orders behind us, and accumulated divergence is reported |
| Cancel race | Order and cancel latencies modelled separately; fills arriving while our cancel is in flight reported as their own bucket |
| Order-type semantics | Post-only rejects, crossing quotes, IOC and odd lots follow Nasdaq rules in the exchange simulator |
| Unseen liquidity | Hidden orders and other venues are invisible; fills whose price was traded through elsewhere are flagged |
| Fill rule | Every result is also reported under a conservative rule that fills only when the price trades through our level; the two rules bound the fill assumption |

### Validation against real orders

- **Implementation check.** A real ITCH order removed from the book and re-inserted as
  a virtual order at its own position must reproduce its actual executions exactly.
  Any mismatch is a queue-tracking bug; this checks the code, not the no-reaction
  assumption, which replay cannot test.
- **Fill model calibration.** The fill model's predicted fill probabilities are
  compared with the realised outcomes of real orders on held-out days.
- **Value of order-level data.** An L2-only queue-position estimator is run on the same
  orders, and its error against the exact position measures what order-level data adds.

### Fill model

Competing-risks survival: a resting order ends by fill or by the price moving away from
its level, each with a cause-specific hazard. Cancels are censoring, not a cause: our
own cancels are policy decisions, and cancels of real training orders are informative
censoring, reported as a caveat. Covariates: queue ahead, queue behind, `I_t`, `s_t`,
signal value, volatility.

### Queue value

`W(pos) = P(fill | pos) * (E[markout | fill, pos] + rebate)` as a function of queue
position (Moallemi, Yuan 2016), used directly by the quoting policy.

### Adverse selection

- Markouts `M(τ) = θ (m_{t+τ} - p)` for a fill at price `p`, τ from 1 ms to 60 s.
- Conditioned on queue position, signal value and fill mechanism: small trade at the
  front of the queue, sweep through the whole level, or fill during an in-flight cancel.

### Implementation

Each order carries its arrival sequence number. A virtual order's queue-ahead count
falls when an order with a lower sequence number at the same level cancels or executes,
an O(1) update per event. Many virtual orders on one level switch to a Fenwick tree.

## 3. Quoting

### Baseline

Avellaneda-Stoikov, with the Guéant-Lehalle-Fernandez-Tapia closed form when
inventory limits apply. With `d` the quote distance from the reservation price:

- Reservation price `r_t = m_t - q_t γ σ² (T - t)`
- Optimal distance `d* = (1/γ) ln(1 + γ/k) + (1/2) γ σ² (T - t)`
- Fill intensity `Λ(d) = A e^{-k d}`, `A, k` fitted from section 2

It assumes continuous prices, Poisson fills and Brownian mid. For large-tick stocks the
spread is one tick most of the time and a continuous `d` has no meaning, so it serves
only as the baseline.

### Core: data-driven optimal control

A discrete Markov decision process in the spirit of Guilbaud and Pham (2013) and
Cartea, Jaimungal and Ricci (2014):

- **State**: inventory, queue-position bucket on each side (or no quote), imbalance
  bucket, spread, signal bucket, time-of-day bucket.
- **Actions**: per side, join, improve, hold or cancel; or take liquidity with a market
  order.
- **Transitions**: estimated from replay using the fill model and queue dynamics of
  section 2.
- **Reward**: spread capture plus rebates minus fees and adverse selection, with an
  inventory penalty.
- **Solution**: exact value iteration; the state space is small enough that the policy
  is optimal within the estimated model.

As a stretch goal, an RL agent (PPO) trained in the simulator is compared against the
DP policy to show whether learning finds structure beyond the model or overfits to it.

### Extensions

| Extension | Description |
|-----------|-------------|
| Aggressive taking | Cross the spread when the signal exceeds half-spread plus fees |
| Deep queue reservation | Rest orders at deeper levels to reserve queue priority before the price moves there; orders are bona fide and fillable, and cancel rates at deep levels are reported |
| Portfolio inventory | Risk penalty `γ qᵀ Σ q` across stocks; hedge inventory with QQQ or correlated names |
| Toxicity guard | Pull or widen when toxic-fill measures spike, around auctions and in volatility bursts |
| Online recalibration | Re-estimate `σ` and fill parameters per stock and regime during the day |
| New-rule policy | DP re-solved at `δ = $0.005` and the 10 mil fee cap, feeding section 5 |

### Implementation

- The DP policy is solved offline and exported as a flat, cache-aligned lookup table; the
  live decision is one indexed load.
- Hysteresis on quote changes prevents churning.
- A message-rate throttle keeps order-to-trade ratios within exchange limits.

## 4. Backtest and attribution

One event-driven engine, running the same strategy, risk and exchange code as the
pipeline.

### Event loop

The scheduler (section 8) pops the next event in (timestamp, sequence) order, so a
cancel that reaches the exchange before an execution takes effect first:

1. **Market event** at exchange time `t`: applied to the exchange-side book, where
   virtual orders fill against real flow at their queue position (section 2).
2. **Market data arrival** at `t` plus the market-data latency: the strategy's view
   and features update, and the strategy decides. New orders pass the risk checks and
   are scheduled to reach the exchange after the processing and order-entry latencies.
3. **Order arrival** at the exchange: the order joins or cancels in the exchange-side
   book.
4. **Report arrival** at the strategy: acknowledgements and fills, one order-entry
   latency after the exchange produced them.

At equal timestamps, real market events go before our orders, the conservative choice
for queue races.

### Latency

Three separate delays, each configurable and swept from 0 to 500 µs:
- market data: exchange to strategy
- order entry: strategy to exchange, and the same delay for reports coming back
- processing: measured from the real code path, or fixed

### Costs

- Nasdaq maker rebate and taker fee by tier, under the 30 mil and 10 mil caps.
- SEC Section 31 fee and FINRA trading activity fee on sells.
- Intraday only; inventory flattened at the close at the real closing book, so no
  overnight risk or borrow.

### Accounting and attribution

Cash and inventory are tracked per fill and marked to mid, in integer
micro-dollars (ITCH prices are $0.0001 and mids $0.00005, both exact). For a fill of size `n` at
price `p` with side `θ`, and `Δm_t` the mid change at event `t`:

```
spread capture       = Σ_fills  θ (m_t - p) n
inventory PnL        = Σ_events q_{t-} Δm_t
fee PnL              = rebates - access fees - regulatory fees
adverse selection    = Σ_fills  θ (m_{t+1s} - m_t) n
```

Total PnL is computed separately, from cash plus inventory marked to the mid. After
every event it must equal spread capture plus inventory PnL plus fee PnL exactly; the
check catches missed fills, sign errors and fee mistakes. Adverse selection is reported
as the part of inventory PnL that follows our fills within 1 s, and the remainder as
residual inventory PnL.

### Strategies compared

| # | Strategy |
|---|----------|
| 1 | Naive: always join best bid and ask, fixed size |
| 2 | Avellaneda-Stoikov |
| 3 | DP policy |
| 4 | DP policy with signals |
| 5 | DP policy with signals and extensions |
| 6 | RL agent (Stretch) |

An ablation table removes one component at a time from strategy 5.

### Sanity checks

- A strategy that never trades has zero PnL.
- Random taking loses about half the spread plus fees per share; random passive
  quoting earns spread capture net of adverse selection, matching the markout curves
  of section 2.
- A strategy given the future mid at zero latency is profitable; it bounds what any
  signal can achieve.
- Accounting identity holds after every event.

### Sensitivity

PnL against latency, order size, fee schedule, and the two fill rules of section 2.

### Protocol

- Train days for fitting, validation days for choices, test days run once at the end.
- Every run is defined by a config file and records the commit hash.
- Output per run: fills, orders and PnL in Parquet, plus a summary report.

### Metrics

Per stock group (large-tick, small-tick): PnL distribution per stock-day, Sharpe and
deflated Sharpe on intraday buckets, max drawdown, fill rate, inventory distribution,
PnL per share, markouts, attribution terms, all with bootstrap confidence intervals
clustered by day.

## 5. Regime forecast (Q2)

Calibrating on current data and halving the tick is an extrapolation, so the forecast
combines three different methods and is checked against past tick changes before it is
frozen. The methods share the same data, so their agreement is not independent
evidence.

### Scope

- **Treated stocks**: price at least $1 and time-weighted average quoted spread at most
  $0.015. The SEC rule uses the consolidated (NBBO) spread over a three-month
  evaluation period. Here the Nasdaq-only spread over the available days serves as a
  proxy; it is at least as wide as the NBBO, so it can misclassify borderline stocks.
  Forecasts are therefore published for every universe stock under both ticks, and
  scoring uses the official list in force at compliance. Untreated stocks act as
  controls for the tick change; the fee cap applies to both groups.
- **Changes modelled**: tick `$0.01 -> $0.005` and access fee cap 30 to 10 mil.
- **Locked/crossed quotes**: the proposed rescission of Rule 610(e) is a cross-venue
  effect that single-venue data cannot observe. It is run as a simulator scenario only
  and reported separately, not as part of the forecast.

### Forecast targets

Per universe stock, under both ticks: quoted spread, depth at best, queue length, fill
probability by queue position, markouts, and market-maker PnL per share for the
strategies of section 4.

### Method A: cross-sectional tick elasticity

Relative tick size (tick divided by price, or by typical spread) varies widely across
stocks today. Regress queue length, spread and fill rates on relative tick size with
controls for volatility, volume and price. Halving the tick moves each treated stock
along these curves.

### Method B: implicit spread model

Dayri and Rosenbaum (2015) measure a large-tick stock's latent spread through `η`, the
ratio of mid-price continuations to alternations, and give a formula for the spread
under a different tick. Applied per stock to predict the new spread and tick
constraint.

### Method C: structural simulation

- Queue-reactive model (Huang, Lehalle, Rosenbaum 2015) with the power-law feedback
  kernel of Noble et al., calibrated per stock.
- Existing depth redistributed across the new half-tick levels, with the redistribution
  rule varied as a sensitivity.
- Lower rebates reduce the value of queue priority, entered through the queue value
  model of section 2.
- The DP policy is re-solved under the new tick and fees, and the full backtest re-run.

### Latent demand evidence

Executions against non-displayed orders at half-penny prices (midpoint fills in ITCH `P`
messages) already occur in treated stocks. Their share measures demand to trade inside
the current tick and is reported alongside the forecasts.

### Validation

- **Leave-stocks-out (cross-sectional fit).** Method A predicts the spread, queue
  length and fill rates of held-out stocks from their relative tick size, using models
  fitted on the other stocks; methods B and C predict held-out days of each stock.
  This checks fit on our own data. It cannot show that changing one stock's tick moves
  it along the cross-sectional curve, which is the step the forecast relies on.
- **Past tick changes (the causal step).** The Tokyo Stock Exchange reduced ticks for
  liquid large caps in 2014, the same direction as the US change; Huang, Lehalle and
  Rosenbaum (2016) showed that an `η`-based model like method B predicted its effects
  ex ante. The SEC Tick Size Pilot (2016 to 2018) widened the tick to $0.05 for small
  caps, the opposite direction and a different population. Raw order data for neither is
  available here, so each method is checked against the direction and rough size of
  the published effects, with inputs taken from the published pre-change statistics.

### Combination and uncertainty

Final forecast is a weighted combination of A, B and C, weights set by leave-stocks-out
performance and consistency with the past tick changes. Uncertainty bands combine
calibration bootstrap within each method and disagreement between methods. Where the
methods disagree, the forecast says so.

### Pre-registration

- Forecasts, code and data hashes frozen under a git tag and published before the
  November 2027 compliance date.
- Forecasts are conditional on market-wide volatility, volume and price, so realised
  values of those are plugged in at scoring and market drift between now and 2027 is
  not scored as forecast error. The tick effect is also scored as a difference between
  treated and untreated stocks.
- Scoring rules fixed in advance: error of the predicted change against the realised
  change per stock, interval coverage, and continuous ranked probability score.
- Scored on the first post-change ITCH sample days Nasdaq publishes, the same source
  as the calibration data. Nasdaq posts sample days irregularly, so the delay is
  stated in advance and the forecast stays open until such a day appears.

## 6. Impact (E2)

The square-root law `ΔP(Q) = Y σ (Q / V_D)^ψ` with `ψ ≈ 0.5` is usually measured on
proprietary metaorder data; Maitrier, Loeper and Bouchaud (2025) recover it from
public trades with synthetic metaorders. This section measures impact from public
order-level data, tests the latent-liquidity explanation of the law, and separates
mechanical from reactive impact.

### Single-trade impact and the propagator

- Response function `R(l) = E[ε_t (m_{t+l} - m_t)]`.
- Trade-sign autocorrelation `C(l) = E[ε_t ε_{t+l}] ~ l^{-κ}`, long memory expected.
- Propagator model (Bouchaud, Gefen, Potters, Wyart 2004):
  `m_t = Σ_{s<t} G(t - s) ε_s + noise`, with `G` recovered from `R` and `C` by solving
  the linear system between them.
- Consistency check: long-memory signs with a decaying `G` must still give diffusive
  prices; the measured Hurst exponent of the mid is reported.

### Metaorder proxies

| Proxy | Construction |
|-------|-------------|
| Attributed orders | ITCH `F` messages carry the market participant ID; sequences of same-side executions from one ID approximate metaorders. Attribution is optional and the ID is a broker, not a client, so its volume share is reported and results are a biased subsample |
| Sign runs | Clusters of same-sign trades, motivated by order splitting (Lillo, Mike, Farmer 2005) |
| Public synthetic | Trades grouped into synthetic metaorders by the method of Maitrier, Loeper and Bouchaud (2025) |
| Injected | Metaorders of known size injected in replay and simulator |

### Square-root fit

- `ψ` estimated by log-log regression over binned `Q / V_D`, with bootstrap intervals.
- Impact path during execution and decay after completion; the ratio of permanent to
  peak impact reported.
- Split by large-tick versus small-tick stocks, and predicted under the half-penny tick
  as an input to section 5.

### Microscopic origin

Latent order book theory (Tóth et al. 2011; Donier, Bonart, Mastromatteo, Bouchaud
2015) explains the square root by a latent liquidity density that vanishes linearly at
the price. It predicts linear impact for metaorders that are small or slow relative to
the renewal time of the latent book, and square-root impact otherwise.

Simulator sweeps over order-sign memory, cancellation rate, liquidity refill rate and
execution speed map `ψ` across this parameter space and test whether the predicted
linear-to-square-root crossover appears where the theory places it.

### Mechanical versus reactive impact

- **Simulator, reaction off**: the metaorder consumes the book while the other flow's
  intensities follow the unperturbed book, under common random numbers, so impact is
  mechanical.
- **Simulator, reaction on**: flow responds to the metaorder, so impact is mechanical
  plus reactive.
- The difference between the two arms estimates the reactive share. Both arms use one
  model, so simulator error does not enter the difference as it would if replay were
  compared with the simulator. Replay serves as a check on the reaction-off arm.

### Execution

Schedules compared in replay and simulator for the same metaorders:
- TWAP and VWAP
- Almgren-Chriss with fitted parameters
- Optimal execution under the fitted transient-impact kernel: Obizhaeva-Wang for an
  exponential kernel, and the numerical solution of Gatheral, Schied and Slynko
  (2012) for a power-law one

Fitted kernel and impact function checked against Gatheral's (2010) no-dynamic-arbitrage
conditions, which rule out round trips with expected profit.

### Implementation

Propagator estimation and convolutions use FFTs; sweeps run in parallel from config
files and write Parquet.

## 7. Counterfactual validity (E1)

Learned simulators are judged on how realistic their generated data looks. A simulator
used for impact estimates, execution or the section 5 forecast must also respond
correctly to interventions. The hypothesis tested here: unconditional realism does not
imply counterfactual validity.

### Interventions

- **State** `X`: a book and event history at time `t`, sampled from ITCH.
- **Intervention** `a`: a market order of size `q`, a limit order at level `k`, a cancel
  of depth at level `k`, or a short sequence of these.
- **Outcome** `Z` over horizon `h`: mid change, spread, depth, fill of a reference order.
- **Effect** `Δ(X, a) = E[Z | X ⊕ a] - E[Z | X]`, estimated from paired Monte Carlo
  rollouts that share random numbers so the difference has low variance.

Models generate events; the deterministic matching engine executes them, so every
subject is held to the same exchange mechanics.

### Ground truth

- Empirical conditional responses: real market orders of size `q` in states like `X`,
  matched on book state, give `Δ` measured in the data.
- Measurements from section 6: response function, propagator, square-root exponent.

Both are observational: real orders carry information, so their measured response
exceeds the effect of an uninformed intervention. A generator that treats an injected
order like an observed one estimates the same observational quantity, so the
comparison is like for like. Levels are therefore read as consistency with observed
responses, not as causal ground truth; the structural relations below hold either way.

### Relations

Each relation is a statistical test with a tolerance set from the empirical confidence
interval.

| Relation | Test |
|----------|------|
| Null intervention | `Δ(X, ∅)` is zero in distribution |
| Side symmetry | `Δ(X, buy q) ≈ -Δ(mirror(X), sell q)` |
| Size monotonicity | Violation rate of `|Δ(q₁)| ≤ |Δ(q₂)|` for `q₁ < q₂` |
| Concavity | Fitted impact exponent within the empirical interval |
| Linear response | `Δ(a₁ + a₂) ≈ Δ(a₁) + Δ(a₂)` for small interventions |
| Decay | Distance between model and empirical `R(l)` |
| Scale collapse | Impact normalised by spread and volatility collapses across stocks as in the data |
| Path dependence | Same visible book, different queue history: outcome distributions differ when the data says they do (two-sample test) |
| Stability | Repeating an intervention with fresh seeds gives the same outcome distribution |
| Mechanical consistency | Zero priority or conservation violations; counted for models that emit book states directly |

### Subjects

| Subject | Type |
|---------|------|
| Replay | No reaction; mechanical baseline |
| Queue-reactive simulator | Parametric, with Noble et al. feedback kernel |
| Event transformer | Our compact model trained as a next-event generator |
| M3 | Released 10M, 25M and 75M checkpoints (non-commercial licence), trained on Chinese CSI 300 and CSI 500 stocks. They enter only after fine-tuning on ITCH, which fits the 8 GB GPU; results are reported as a different-market model adapted to Nasdaq |
| Forecaster-based impact | Linna et al. method reproduced: inject messages into a forecaster and compare predictive distributions |

### Stress tests

- Out-of-distribution interventions: very large orders, repeated cancels, layered
  orders followed by cancels.
- Long-rollout derailment: divergence from real statistics as a function of horizon.

### Outputs

- LOB-Bench realism scores per subject.
- Pass/fail matrix of subjects against relations, with effect sizes.
- Realism against validity plot. With a handful of subjects this is not a correlation
  test; the hypothesis is an existence claim, shown by one subject that scores well
  on realism and fails relations.

### Implementation

- Relations form a test harness in the style of property-based testing: a relation
  interface, a state generator sampling from data, and shrinking of failing cases to a
  minimal counterexample (smallest intervention and shortest history that still fail).
- Paired rollouts run in parallel with common random numbers.
- The same relations run nightly on the self-hosted runner, which has the data, as
  regression tests for our own models.

---

# Requirements

Derived from Part I. Target machine, the only one available:

| Part | Specification |
|------|---------------|
| CPU | Intel i7-13650HX: 6 performance cores (48 KB L1 data, 1.25 MB L2 each) and 8 efficiency cores (32 KB L1 data, 2 MB L2 per four-core cluster), 20 threads, 24 MB L3, AVX2 and AVX-VNNI, no AVX-512 |
| Memory | 24 GB DDR5-4800, dual channel |
| GPU | NVIDIA RTX 4060 Laptop, 8 GB, usable from WSL2 through CUDA |
| Disk | One 512 GB NVMe SSD, about 400 GB free |
| OS | Windows 11, WSL 2 with Microsoft's 6.18 kernel, Ubuntu 24.04; no native Linux |

## Functional

| ID | Requirement | Source |
|----|-------------|--------|
| D1 | Ingest each day once into a fast seekable store partitioned by symbol; no permanent 30 to 50 GB raw files | all |
| D2 | Book checkpoints every N events so any state at time `t` is reachable quickly | §2, §6, §7 |
| D3 | Retain participant IDs (`F`), non-displayed executions (`P`) and imbalance messages (`I`) | §1, §6 |
| D4 | Reference data: QQQ weights, fee schedules, half-penny eligibility, published results of the Tick Size Pilot and the 2014 Tokyo tick reduction | §1, §4, §5 |
| D5 | Day split registry with the test set locked | §1, §4 |
| C1 | L3 order book with order IDs and arrival sequence numbers | §2 |
| C2 | Virtual orders inside the real queue, with divergence tracking | §2 |
| C3 | Configurable tick size, fees, Nasdaq order types, self-trade prevention, locked/crossed policy | §2, §4, §5 |
| C4 | Snapshot, fork and restore of engine state | §6, §7 |
| C5 | Simulation mode driven by model-generated events instead of ITCH | §5, §6, §7 |
| C6 | Discrete-event scheduler merging market events with strategy messages under three latencies | §4 |
| C7 | Multi-symbol strategy view for cross-asset signals and hedging | §1, §3 |
| F1 | O(1) streaming features, same code in batch and live | §1 |
| F2 | Leakage-proof feature API; targets produced only by a separate offline labeler | §1 |
| F3 | Feature and target export to Parquet | §1 |
| F4 | Python training: ridge, LightGBM, Hawkes MLE, competing-risks survival, transformer, PPO, DP, simulator calibration | §1, §2, §3, §5 |
| F5 | C++ serving: linear models, online RLS, tree ensembles, transformer kernel, DP table | §1, §3 |
| F6 | Gym-style environment over the engine for PPO | §3 |
| F7 | Versioned model artifacts (weights plus hash) | §5 |
| X1 | Sweep orchestrator for thousands of runs (strategies × stocks × days × latencies × fees) | §4, §5, §6 |
| X2 | Experiment registry logging every run, feeding the trial count for the deflated Sharpe | §1, §4 |
| X3 | Counter-based RNG (Philox) with draws addressed by (pair, rollout, event, purpose), so paired arms stay synchronised after an intervention | §6, §7 |
| X4 | Property-based relation harness with shrinking | §7 |
| X5 | Statistics: bootstrap, deflated Sharpe, purged CV, CRPS | §1, §4, §5 |
| X6 | Pre-registration bundle: code, data hashes, scoring scripts, git tag | §5 |
| X7 | FFT propagator estimation and synthetic metaorder injection | §6 |

## Non-functional

| ID | Requirement | Target |
|----|-------------|--------|
| N1 | Replay throughput | Full day, 50 stocks, under 60 s with one replay thread and one decompression thread |
| N2 | Fork cost | Bound by memory bandwidth: copy at 10 GB/s or more; under 100 µs for a symbol with 10k resting orders (under 1 MB of state, its order-ID map included) |
| N3 | Simulation rollout rate | At least 1M events/s across cores for the C++ agent source; learned generators are bound by their own inference |
| N4 | Memory | WSL2 limited to 18 GB, leaving about 6 GB for Windows. Under 2 GB per replay or simulation run, so a sweep runs eight or more at once; under 14 GB for any single job, training included |
| N5 | Disk | Under 250 GB at peak for all days, features and one in-flight download |
| N6 | Determinism | Same input, config, seed and build give byte-identical output |
| N7 | Correctness | Book invariants and PnL identity after every event |
| N8 | Reproducibility | Every result traceable to config, commit, data hash and model hash |
| N9 | Hybrid CPU | Threads pinned to WSL2 virtual CPUs only. The host moves virtual CPUs between performance and efficiency cores, so no per-core-type result is claimed and run-to-run spread is reported instead |
| N10 | Live path (Stretch) | Tick-to-trade p99 measured and reported; no hard target under WSL2 |

## Risks

| Risk | Mitigation |
|------|-----------|
| Few days of data | Consecutive December week for train/validation/test, other days as out-of-sample checks, intervals reported everywhere |
| Full days are 30 to 50 GB raw, over a billion messages | Ingest splits the stream by stock-locate code, which every ITCH 5.0 message carries, so a replay reads only its symbols and merges them by sequence number |
| gzip decompression around 300 MB/s | Decompress once, streaming, with zlib-ng or ISA-L `igzip` into a chunked zstd store; libdeflate needs whole buffers in memory and cannot hold a 50 GB day |
| No AVX-512 | Kernels target AVX2 |
| WSL2 defaults | `.wslconfig` sets `memory=18GB` (the default is half the host's 24 GB); data kept on the WSL2 ext4 disk, never under `/mnt/c`; the WSL2 virtual disk grows to peak use and does not shrink by itself, so N5 is budgeted at peak |
| WSL2 only: no control of core type, noisy timing | No per-core-type claims; instruction counts back every wall-clock comparison; caveats on all latency figures |
| Kernel features under WSL2 | Microsoft's 6.18 WSL2 kernel config enables AF_XDP sockets, BPF, hugetlbfs, transparent huge pages in `madvise` mode, `io_uring`, busy polling, `veth` and network namespaces; rechecked from `/proc/config.gz` at setup |
| Training data larger than memory | Event-level features for 50 stocks run to hundreds of millions of rows per day; training sets are sampled (every k-th event or on a clock grid) to fit in the 14 GB job limit, and full streams are recomputed by the same C++ code when needed |

## Threats to validity

| Threat | Effect | Mitigation |
|--------|--------|-----------|
| Single venue | ITCH shows Nasdaq only, about 15 to 20% of US volume; signals and queue states are partial | Stated with every result; how much predictability survives on one venue is reported as a finding |
| Few days | About 20 days, five consecutive; wide intervals, risk of day-specific results | Bootstrap intervals clustered by day, held-out test day, out-of-sample 2026 days, claims sized to intervals |
| Older data | 2019 to 2021 days come from different regimes | Used only for robustness, never for fitting the main results |
| No reaction in replay | Real participants do not respond to virtual orders, so impact and fills are optimistic | Small order sizes, divergence tracking, queue-reactive simulator as a sensitivity check |
| Unseen liquidity | Hidden orders and other venues can fill or trade through without trace | Flagged fill bucket, conservative trade-through fill rule |
| Model extrapolation | The regime forecast moves outside observed tick conditions | Three methods, checks against the 2014 Tokyo tick reduction and the Tick Size Pilot, disagreement shown in intervals |
| Selection bias | Many signal and strategy variants tried on the same days | Registry trial count, deflated Sharpe, test set run once |
| Leakage | Features using future information inflate results | Leakage-proof feature API, separate labeler, future-perturbation test |
| Market drift | Volatility and volume in 2027 differ from the calibration days | Forecasts conditional on market-wide covariates; tick effect also scored against untreated stocks |
| Simulator dependence | Sections 5 to 7 rely on simulators that may be wrong | Parameter recovery tests, LOB-Bench scores, metamorphic relations against data |
| Timing environment | WSL2 virtualisation adds noise and blocks some kernel features | Caveats on all latency figures, instruction counts for regression, run-to-run spread reported |
| Fee assumptions | Actual rebate tiers depend on volume a single strategy would not reach | Results reported under several tiers and the 10 mil cap |
| Rule changes before compliance | The November 2027 date rests on an exemptive extension, and the Rule 611 proposal could alter the tick and fee rules | SEC releases checked before freezing; forecast scope updated and the change documented |
| Eligibility approximation | The rule uses a three-month NBBO average; only Nasdaq-only spreads on sample days are available | Forecasts published for every stock under both ticks; scored on the official list; borderline stocks reported separately |
| Data licence | Nasdaq sample data terms govern publication | Terms checked before publishing; only derived statistics published, never raw data |

---

# Part II: Engineering

## 8. System

### Layers

```
 ┌──────────────────────────── Experiment layer ─────────────────────────────┐
 │ configs (TOML) -> orchestrator (process pool) -> runs -> registry (SQLite) │
 └──────────────────────────────────┬─────────────────────────────────────────┘
                                    │
 ┌─────────────── Research interface: nanobind module + Python package ───────┐
 │ engine, sources, Gym environment, features │ training, statistics, plots   │
 └──────────────────────────────────┬─────────────────────────────────────────┘
                                    │
 ┌────────────────────────────── Runtime ─────────────────────────────────────┐
 │  Event sources          Core engine                 Strategy               │
 │  ─────────────          ───────────                 ────────               │
 │  ReplaySource  ──┐      Scheduler (time, seq)  ──>  FeatureEngine          │
 │  AgentSource   ──┼──>   L3 Book                     Models                 │
 │  ExternalSource──┘      MatchingEngine         <──  Policy -> Risk         │
 │                         Snapshot / fork             -> Gateway             │
 └──────────────────────────────────┬─────────────────────────────────────────┘
                                    │
 ┌────────────────────────────── Data layer ──────────────────────────────────┐
 │ ingest -> chunked zstd store + index │ book checkpoints │ reference data   │
 │ split registry (test days locked)                                          │
 └────────────────────────────────────────────────────────────────────────────┘
```

### Data layer

- **Ingest.** Each ITCH day is decompressed once, streaming, with zlib-ng or ISA-L,
  framed, split by stock-locate code and written as a chunked zstd store per symbol of
  about 1M messages per chunk. Every message keeps its global sequence number, so
  symbols merge back in feed order. An index maps each chunk to its file offset, first
  timestamp and first sequence number.
- **Checkpoints.** Per-symbol book snapshots every N events, stored beside the chunks,
  so any time `t` is reached by loading a checkpoint and replaying one chunk.
- **Reference data.** QQQ weights, fee schedules, half-penny eligibility and published
  results of past tick changes as versioned files with hashes.
- **Split registry.** Assigns days to train, validation and test. Opening a test day
  requires an explicit flag and is logged in the experiment registry.

### Core engine

- **L3 book.** Every order with its ID and arrival sequence number; virtual orders
  share the same queues and are flagged.
- **Matching engine.** Price-time priority with configurable tick size, fee schedule,
  Nasdaq order types (post-only, IOC, odd lots), self-trade prevention and
  locked/crossed policy. Tracks divergence caused by virtual fills.
- **Scheduler.** Discrete-event queue ordered by (timestamp, sequence number) with a
  deterministic tie-break. Merges market events with strategy messages delayed by the
  three latencies of section 4.
- **Snapshot and fork.** All engine state lives in per-symbol pools addressed by 32-bit
  indices instead of pointers, so the state is trivially copyable and a fork is a
  `memcpy`. The order-ID map is part of each symbol's state, so it forks with it.
  Indices also halve link size against 64-bit pointers.

### Event sources

One compile-time interface (C++20 concept), three implementations:
- `ReplaySource`: reads the ITCH store.
- `AgentSource`: queue-reactive and Hawkes order flow implemented in C++.
- `ExternalSource`: events generated by Python models (event transformer, M3), passed
  through the bindings in batches.

### Strategy

- **FeatureEngine.** Receives a read-only `MarketView` of all symbols at the current
  time and nothing later. Targets come only from a separate labeler tool that reads the
  store forward, so features cannot see the future by construction.
- **Models.** Linear, online RLS, tree ensembles in a flat array layout, transformer
  kernel (section 11), DP lookup table.
- **Policy.** Quoting logic of section 3.
- **Risk.** Max position, max order size, price collars, order-rate throttle,
  self-trade prevention, kill switch; checked on every order, latency measured.
- **Gateway.** In-process for batch and simulation; Nasdaq OUCH 5.0 over TCP for the
  pipeline.

### Runtime modes

The same strategy, risk and engine objects run in all modes; only the source, gateway
and clock change.

| Mode | Source | Gateway | Clock | Use |
|------|--------|---------|-------|-----|
| Batch | Replay | In-process | Simulated | Research and backtests, deterministic |
| Simulation | Agent or external | In-process | Simulated | Sections 5 to 7 |
| Environment | Replay or agent | In-process | Simulated | Gym `step` / `reset` for PPO |
| Pipeline (Stretch) | MoldUDP64 feed | OUCH over TCP | TSC | Tick-to-trade measurement |

Pipeline mode runs one thread per stage (feed handler, book, strategy, gateway), each
pinned to its own virtual CPU, connected by SPSC ring buffers. A separate exchange
process owns the authoritative book: it publishes the replayed feed over MoldUDP64 and
accepts OUCH orders against the same book, so feed and matching never disagree.

### Experiment layer

- **Configs.** Every run is a TOML file: data days, symbols, strategy, model artifacts,
  latencies, fees, seed.
- **Orchestrator.** Runs are single-threaded and independent, so sweeps are a process
  pool across cores. The pool size is the smaller of the thread count and the memory
  limit divided by each run's measured peak (N4), so a sweep never swaps. Latency
  benchmarks never run alongside a sweep.
- **Registry.** SQLite table with run ID, config hash, commit, data hash, model hashes,
  metrics and output paths. The count of runs per research question feeds the deflated
  Sharpe ratio.
- **Randomness.** Philox counter-based RNG keyed by (experiment seed, pair ID, rollout
  ID), with the counter set by (event index, draw purpose). The intervention arm is not
  part of the key, so both arms of a pair draw the same numbers for the same event even
  after the intervention changes how many draws each makes. Any rollout is
  reproducible in isolation.
- **Model artifacts.** Flat binary weights with a header holding shape, version and
  hash; loaded into aligned memory.

### Design rules

- No virtual calls on hot paths; sources, gateways, receive backends and models are
  selected at compile time.
- No heap allocation after startup.
- Determinism in batch and simulation modes: single thread per run, ordered scheduler,
  no iteration over unordered containers in outputs, seeded counter-based RNG.

## 9. Performance engineering

Every optimisation lands with a before/after measurement under the methodology below.
Targets come from the non-functional requirements.

### Targets

| Component | Metric | Target | Requirement |
|-----------|--------|--------|-------------|
| Store read + decode | Messages per second, merge and decode thread fed by two decompression threads | ≥ 30M | N1 |
| Book update, one symbol | Median / p99 per event | ≤ 55 ns / ≤ 160 ns | N1 |
| Book update, 50 symbols interleaved | Median / p99 per event | ≤ 85 ns / ≤ 400 ns | N1 |
| Full-day replay, 50 stocks | Wall time, one replay thread | ≤ 60 s | N1 |
| Single-symbol fork | Copy bandwidth; time for 10k resting orders | ≥ 10 GB/s; ≤ 100 µs | N2 |
| Agent simulation | Events per second, all cores | ≥ 1M | N3 |
| SPSC hop | Median latency between two pinned virtual CPUs | Measured, with run-to-run spread | N9 |

Targets are revised after the first baseline; misses are reported, not hidden. All figures
are at the baseline clock (turbo off, about 2.4 GHz).

### Target revisions

Revision 1 (2026-10-08), after the wall-clock baseline (2026-10-07) and one optimisation
pass. The first targets were set from runs with turbo on (about 4.7 GHz); the baseline
fixes the clock at about 2.4 GHz, which roughly doubles every time figure for the same
cycles. Evidence: `docs/results/ms2-book-study.md`, `docs/results/ms9-transformer.md`.

| Target | Old | Baseline | Final | New | Reason |
|---|---|---|---|---|---|
| Read + decode | ≥ 50M msg/s, one decompression thread | 18.8M decode thread alone, 17.8M pipeline | 32.3M decode thread alone, 31.6M pipeline with two decompression threads | ≥ 30M, two decompression threads | 50M at 4.7 GHz is 94 cycles per message; 32.3M at 2.4 GHz is 74. One zstd thread decompresses 17.4M/s of this store (41 bytes per message), so the pipeline needs two; a third and fourth add nothing. |
| Book update, one symbol | ≤ 30 / ≤ 150 ns | 53.5 / 152 ns | 52.8 / 140 ns | ≤ 55 / ≤ 160 ns | In cycles the old target is 141 / 705 and the final 127 / 336: met. Remaining cost is about 250 instructions and 3 L1 misses per event; instruction-level changes beyond one ID probe per operation measured at under 1% or worse. |
| Book update, 50 symbols | (not separate) | 81.0 / 366 ns | 79.6 / 367 ns | ≤ 85 / ≤ 400 ns | Fifty interleaved books keep about 100 MB of ID maps and orders, outside the caches; the figure measures that working set and is tracked on its own. |
| Full-day replay, 50 stocks | ≤ 60 s | 25.9 s | 23.9 s | unchanged | Met. |
| Fork, 10k orders | ≤ 100 µs, ≥ 10 GB/s | 53.4 µs, 16.0 GiB/s | not re-measured | unchanged | Met. |
| Transformer step (section 11) | < 2 µs per event | small 2,935 ns, base 3,435 ns | small forecast step 1,975 ns, full step 2,140 ns; base 2,373 / 2,545 ns | < 2 µs for the in-loop forecast step of the small model | The trading loop needs only the forecast heads; the generator head serves simulation and is reported beside it. The base model (2x the parameters and the 40 KB budget) is reported, not targeted. |

### Methodology

- Threads pinned to virtual CPUs. The host may place them on either core type, so
  each figure reports its spread across runs, and core type is never claimed (N9).
- Laptop on mains power; turbo disabled through the Windows power plan (maximum
  processor state 99%) for before/after comparisons.
- Warm-up runs discarded; at least 10 repetitions; median with bootstrap interval.
- Before/after differences tested with a Mann-Whitney test, not eyeballed.
- Hardware counters (cycles, instructions, L1/LLC misses, branch misses) read per code
  region through `perf_event_open`, plus `perf stat` and flamegraphs for whole runs.
  WSL2 exposes a virtual PMU (`hardwarePerformanceCounters`, on by default on x64);
  events it lacks are reported as missing, not estimated.
- Pipeline latency under load measured from the intended send time, avoiding
  coordinated omission.
- The cost of the timing calls themselves is measured and subtracted.
- WSL2 cannot fix CPU frequency or core placement, so all figures carry a
  virtualisation caveat, and instruction counts back every wall-clock comparison.

### Decoder and data path

- Zstd chunks decompressed by a read-ahead thread into a ring of buffers; the replay
  thread stays single and deterministic.
- Zero-copy reads of packed big-endian structs with `std::byteswap`, `switch` dispatch
  into templated handlers, `[[likely]]` on the dominant message types.
- SIMD framing scan versus scalar.
- Software prefetch of the next message batch.

### Order book

- **Price levels**: dense array window around the mid, re-centred when price drifts,
  with a bitmap of non-empty levels; the next level is found with `std::countr_zero` or
  `std::countl_zero` (one `tzcnt` or `lzcnt` per 64-level word). Orders far outside
  the window, such as stub quotes, live in an overflow map.
- **Orders**: hot fields (price index, quantity, next and previous index) packed into
  16 bytes; cold fields (participant ID, timestamp) stored separately. Hot/cold split
  versus a single struct, and structure-of-arrays versus array-of-structs, both measured.
- **Order-ID lookup**: open addressing with linear probing versus Robin Hood hashing
  versus a direct-mapped window over recent reference numbers, which Nasdaq assigns in
  increasing order, with a hash fallback for old orders. A direct array over the whole
  day would need gigabytes and could not fork per symbol.
- **Baselines**: `std::map`, B-tree and sorted vector books, all producing identical
  output.

### Snapshot and fork

- Per-symbol state contiguous in index-addressed pools; fork is a `memcpy` of the
  used region.
- Compared against process-level copy-on-write (`fork()`) for large multi-symbol states.

### Memory

- No heap allocation after startup, enforced by a counting allocator in tests.
- Pools backed by transparent huge pages: `mmap`, then `madvise(MADV_HUGEPAGE)`, then
  pre-fault with `MADV_POPULATE_WRITE`. `MAP_POPULATE` would fault 4 KB pages before
  the advice applies. TLB misses measured with and without. Under WSL2 the host may
  back guest memory with small pages, so the measured gain can understate a native one.
- `alignas(64)` on hot structs, padding against false sharing, with a deliberate
  false-sharing benchmark to show the cost.

### Concurrency

- Bounded SPSC ring buffer: head and tail on separate cache lines, each side caching
  the other's index, batched publish.
- Compared against `boost::lockfree::spsc_queue` and a mutex-plus-condition-variable
  queue.
- Latency between pinned virtual CPUs, with its spread across runs.
- Busy-polling consumers pinned to dedicated virtual CPUs.

### Simulation

- Philox random numbers generated eight at a time with AVX2.
- Agent event generation and matching profiled against the 1M events/s target.

### Build

- `-O2`, `-O3`, `-march=native`, LTO, profile-guided optimisation and post-link layout
  optimisation with BOLT, each measured on the full-day replay. BOLT profiles come from
  instrumentation, since WSL2 may not expose LBR sampling.

### Timing

- `rdtscp` with `lfence` serialisation, invariant TSC verified at startup, calibrated
  against `steady_clock`.
- HDR histograms for all latency distributions.

### Reported metrics

Throughput, per-stage and tick-to-trade p50 / p99 / p99.9 / max, cycles per event, IPC,
cache, TLB and branch misses, allocations, memory footprint. Replay figures measure
processing cost, not wire latency.

## 10. Network I/O: six receive paths (Stretch)

This section is a stretch goal, built after MS11 or when milestones finish early.

### Feed publisher

- Replays the ITCH store as MoldUDP64 multicast at recorded timestamps, accelerated by
  a factor, or as fast as possible; pacing by busy-waiting on the TSC.
- Publishes two redundant lines (A and B) with independent, configurable loss.
- Embeds a TSC send timestamp in a packet trailer; publisher and receiver share one
  host clock, so one-way latency is measured directly.
- Retransmission server answering MoldUDP64 re-request packets over unicast.
- Publisher and receiver run in two network namespaces joined by a `veth` pair, with a
  route for `224.0.0.0/4` on it, so every backend, AF_XDP and DPDK included, sees the
  same path. Loopback cannot serve as the common path, since AF_XDP and DPDK attach to
  an interface.

### Feed handler

- **Line arbitration**: merges lines A and B by sequence number, taking the first copy
  of each message, as production feed handlers do.
- **Gap recovery**: a gap missing on both lines triggers a re-request; messages behind
  the gap are buffered and applied in order once filled.
- **Burst handling**: UDP has no backpressure, so socket buffer size (`SO_RCVBUF`) and
  drop counters are tracked; the market-open burst is replayed as a stress case.

### Receive backends

Interchangeable behind one compile-time interface, each benchmarked on the same
stream:

| Backend | Mechanism |
|---------|-----------|
| `recvfrom` | One system call per packet, baseline |
| `recvmmsg` | Batched receive, many packets per system call |
| `SO_BUSY_POLL` | Kernel busy-polls the socket instead of sleeping on interrupts |
| `io_uring` | Shared submission and completion rings, fewer system calls |
| AF_XDP | Delivery to a user-space ring on the `veth` pair in copy mode; zero-copy needs a physical NIC driver, out of scope |
| DPDK | Full user-space networking via `net_af_packet`, `net_tap` or `net_pcap` virtual drivers |

Socket backends read software receive timestamps (`SO_TIMESTAMPING`) to split kernel
from user-space latency; AF_XDP and DPDK bypass the socket layer and use the TSC
trailer alone.

### Order entry

- OUCH 5.0 runs over SoupBinTCP, Nasdaq's session layer: login, heartbeats, sequenced
  messages and replay of missed messages on reconnect. Both sides implemented.
- Gateway compares blocking TCP, `TCP_NODELAY` with busy polling, and `io_uring`.

### Verification

Every backend must deliver a message stream byte-identical to reading the store
directly, including under injected loss and reordering.

### Metrics and caveats

Packets per second, one-way latency percentiles, CPU per packet, drops under burst,
recovery time after a gap. On virtual interfaces these measure software path cost,
not wire latency. AF_XDP needs a small BPF redirect program, `CAP_NET_RAW` for the
socket and `CAP_BPF` with `CAP_NET_ADMIN` to attach the program; DPDK needs a hugepage
mount. Microsoft's 6.18 WSL2 kernel enables both, and the capabilities are available
through `sudo` inside WSL2, so no custom kernel is needed.

## 11. In-loop inference kernel

Run the event transformer inside the strategy thread without a framework, fast enough
that its latency costs less edge than its prediction adds.

### Model

| Part | Choice |
|------|--------|
| Input per event | Type, side, price offset from mid in ticks, size bucket, log time since previous event |
| Size | `d_model` 16 to 32, 1 to 2 layers, 2 heads, window of 64 to 128 events, about 10k parameters |
| Position | ALiBi-style distance bias, so a sliding window needs no recomputation; trained with the same sliding-window causal mask in every layer, so cached keys and values equal a full recompute |
| Forecast head | Down / flat / up distribution at several horizons; the signal for section 1 |
| Generator head | Next-event distribution; the generator for sections 5 to 7 |

One model serves as both signal and simulator, trained in PyTorch with a shared trunk.

### Kernel

- **Weights**: flat binary artifact (section 8), loaded into 64-byte aligned memory.
  About 40 KB in float32 or 10 KB in int8, within the 48 KB L1 data cache of a
  performance core (32 KB on an efficiency core, where WSL2 may also place the
  thread). Each symbol's key/value cache adds up to 64 KB in float32 (128 events, 2
  layers, `d_model` 32), so one symbol's working set fits the 1.25 MB L2, and the
  caches of all symbols (about 10 MB for 150) live in the 24 MB L3.
- **Incremental inference**: per-symbol key/value cache stored as a ring buffer of the
  window; each event computes one new query, key and value and attends over the ring.
- **Numerics**: max-subtracted softmax, polynomial `exp` approximation with a measured
  error bound, RMSNorm, tanh-approximated GELU.
- **int8 path**: per-channel weight quantisation with int32 accumulation using AVX-VNNI
  (`vpdpbusd`), which this CPU supports. The kernel checks for `avx_vnni` at startup,
  since WSL2 passes through only what Hyper-V exposes, and falls back to AVX2 int8.
- **Determinism**: single thread, fixed reduction order, bit-identical across runs.

### Variants compared

| Variant | Purpose |
|---------|---------|
| Scalar C++ | Reference |
| AVX2 float32 | Main path |
| AVX2 int8 | Without VNNI |
| AVX-VNNI int8 | Fastest path |
| ONNX Runtime, LibTorch | Framework baselines, showing overhead at this model size |

Target: under 2 µs per event for the incremental forecast update of the small model, at the
median across runs, at the baseline clock (revised 2026-10-08, section 9).

### Validation

- Golden tests: recorded inputs and PyTorch outputs; maximum absolute error reported.
- Decision agreement: share of events where the kernel and PyTorch give the same sign
  and bucket, which matters more than raw error.
- Quantisation loss in IC and in backtest PnL, not only in logits.

### Latency in the loop

- Inference time is added to processing latency in the backtest, so the model pays for
  its own cost.
- Per-symbol event rates at the open are compared against inference cost; when the
  strategy falls behind, a policy decides whether to skip stale events or batch them,
  and both are measured.
- Result: PnL with the model against its latency, on the PnL-against-latency curve of
  section 4.

## 12. Verification

An incorrect engine produces convincing nonsense, so every layer, including the
statistics, has its own checks. Checks run in three tiers: **commit** (minutes),
**nightly** (full-day data) and **milestone** (before any result is reported). Checks
for Stretch components, such as the network protocols and the pipeline, start once
those components exist.

### Protocols

| Check | Method | Tier |
|-------|--------|------|
| Spec conformance | Hand-built byte vectors from the ITCH, MoldUDP64, SoupBinTCP and OUCH specifications | commit |
| Independent decoder | Message counts and fields compared against a third-party open-source ITCH parser | nightly |
| Fuzzing | libFuzzer on the ITCH decoder, MoldUDP64 framing, SoupBinTCP and OUCH parsers | commit (short), nightly (long) |
| Backend equivalence | Every receive backend yields a stream byte-identical to the store, under loss and reordering | nightly |

### Book and engine

| Check | Method | Tier |
|-------|--------|------|
| Invariants | After every event in debug builds: no persistent crossed book, level quantity equals the sum of resting orders, no dangling order IDs | commit (slice), nightly (full day) |
| Differential | All book implementations produce the same BBO stream | commit |
| Independent reference | A slow Python book written separately from the C++ code, compared on sampled symbols | nightly |
| Reconciliation | Executed volume per symbol against trade messages | nightly |
| Property tests | RapidCheck: random order sequences against a simple reference matching engine; random scenarios against the risk module | commit |
| Known-answer scenarios | Hand-written event sequences with exact expected fills, queue positions and PnL | commit |
| Fork | A forked state replayed forward matches the original replayed forward | commit |
| Determinism | Byte-identical output by hash within one toolchain; numeric tolerance across GCC and Clang | commit |

### Concurrency and protocols as state machines

| Check | Method | Tier |
|-------|--------|------|
| SPSC ring buffer | Exhaustive check under the C++ memory model with GenMC; TSan stress runs with randomised thread delays | commit (TSan), milestone (GenMC) |
| Session and recovery logic | SoupBinTCP session and MoldUDP64 gap recovery specified in TLA+ and model-checked with TLC; the C++ state machines mirror the spec | milestone |
| Batch versus pipeline | Pipeline mode logs every input to the strategy in arrival order; batch mode replaying that log must produce identical orders. Live timing differs between runs, so the same market data alone cannot be expected to give the same orders | nightly |

### Estimators and models

| Check | Method | Tier |
|-------|--------|------|
| Parameter recovery | Every estimator (Hawkes MLE, survival model, propagator, impact exponent, `η`) fitted on simulated data with known parameters must recover them within tolerance | commit |
| Solver cross-checks | DP against brute force on a tiny state space; Avellaneda-Stoikov closed form against a numerical HJB solution | commit |
| Statistics library | Bootstrap, deflated Sharpe and CRPS against reference values from SciPy and published examples | commit |
| Inference kernel | Golden tests and decision agreement against PyTorch (section 11) | commit |
| Feature consistency | Batch and streaming features identical | commit |

### Research integrity

| Check | Method | Tier |
|-------|--------|------|
| Leakage | Features unchanged when events after `t` are perturbed (section 1) | commit (slice), milestone (full) |
| Queue tracking | Real orders re-inserted as virtual orders reproduce their actual executions exactly (section 2) | nightly |
| Fill model | Predicted fill probabilities against realised outcomes of real orders on held-out days (section 2) | milestone |
| Accounting | PnL identity after every event, against independently computed total PnL (section 4) | commit |
| Backtest sanity | Zero, random and perfect-foresight strategies (section 4) | nightly |
| Test-set lock | Registry audit shows no test-day access before the final run | milestone |
| Pre-registration | Frozen bundle hashes match the published forecast | milestone |

### Code quality

- ASan, UBSan and TSan builds; `-Wall -Wextra -Werror`; clang-tidy and cppcheck.
- Line and branch coverage with llvm-cov, target at least 90% for `core`, `book`,
  `engine` and `feed`.
- Mutation testing with Mull on the same modules, so coverage reflects tests that
  actually detect faults.

## 13. Infrastructure

### Build

- CMake presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `pgo-gen`, `pgo-use`.
- Dependencies pinned with vcpkg manifest mode; `ccache` for rebuilds.
- `clang-format` and `clang-tidy` enforced by pre-commit hooks.
- Python environment managed by `uv` with a lockfile; the nanobind module built with
  scikit-build-core.

### Test data

Nasdaq sample files are large and their redistribution terms are unclear, so none are
committed. CI uses synthetic, spec-valid ITCH fixtures generated by `AgentSource` and
committed as small zstd files. Real-data checks run on the local machine.

### Continuous integration

Two runners, because hosted machines have neither the data nor stable timing:

| Runner | Runs | Tier |
|--------|------|------|
| GitHub-hosted | GCC and Clang build matrix, unit and property tests, sanitizers, short fuzzing, determinism hash on fixtures, instruction-count benchmarks | commit |
| Self-hosted (local WSL2) | Full-day replay with invariants, long fuzzing, wall-clock benchmarks on pinned virtual CPUs, mutation testing; backend equivalence once the Stretch pipeline exists | nightly |

The self-hosted runner only takes scheduled and push-to-main jobs. Pull requests from
forks of a public repository would otherwise run arbitrary code on the local machine.
Nightly jobs need the laptop awake and on mains power; a missed night runs at the next
wake.

TSan jobs set `vm.mmap_rnd_bits=28`, since older TSan runtimes, GCC 13's among them,
fail under the 32-bit ASLR entropy of recent Ubuntu kernels.

**Performance gate.** Hosted runners compare instruction counts from Cachegrind, which
are stable on shared machines; a rise past a set threshold fails the build. The
self-hosted runner tracks wall-clock and latency percentiles over time and flags
regressions.

### Data pipeline

- `hft ingest <day>` downloads a day, verifies it against Nasdaq's `.md5sum` file where
  one is published (only the 2018 to 2020 files have one), otherwise relies on the
  gzip CRC-32 checked during decompression and records its own SHA-256, then writes
  the zstd store, index and checkpoints.
- Data directory outside the repository; every derived file records the hash of its
  input.

### Experiments and results

- TOML configs validated against a schema before a run starts.
- Registry (section 8) as the single source of truth; README result tables generated
  from it, never edited by hand.
- A static HTML report per milestone: tables, plots and the configs that produced them.
- Milestones tagged in git; model artifacts and result tables archived with their hashes.

### Runtime observability

- Low-latency logging: the hot path writes fixed-size binary records into an SPSC ring;
  a background thread formats and writes them.
- Per-stage counters and latency histograms exported at the end of each run.

### Documentation

- `DESIGN.md` (this file) for architecture.
- Architecture decision records in `docs/adr/` for choices such as index-addressed
  pools, compile-time dispatch and the data split.
- README with results, reproduction commands and caveats.
- M3 checkpoints carry a non-commercial licence and are downloaded, not committed.

## Milestones

| # | Deliverable | Done when |
|---|-------------|-----------|
| MS0 | Toolchain, CMake presets, vcpkg, hosted CI with sanitizers | CI green with one test and one benchmark |
| MS1 | Data layer and decoder: ingest, zstd store, index, checkpoints, fuzzing | Message counts match an independent parser; checksums verified |
| MS2 | Order book study and invariants; self-hosted nightly runner | All implementations and the Python reference agree; comparison table with perf counters |
| MS3 | Engine: matching, scheduler, virtual orders, snapshot and fork; configs, registry, RNG | Property, known-answer, fork and determinism checks pass |
| MS4 | Feature engine, labeler, signals (section 1) | IC and decay table on validation days; leakage check passes |
| MS5 | Fills and adverse selection (section 2) | Real orders reproduced exactly; fill model calibrated on held-out real orders; markout curves |
| MS6 | Quoting and backtest (sections 3, 4) | Attribution with confidence intervals, latency sweep, ablations |
| MS7 | Simulation mode with the queue-reactive simulator; regime forecast (section 5) | Simulator passes parameter recovery; checks against past tick changes done; pre-registration bundle published |
| MS8 | Impact and execution (section 6) | Propagator, square-root fit, mechanical versus reactive split |
| MS9 | Event transformer and inference kernel (section 11) | Transformer trained as signal and generator; kernel matches PyTorch; latency table |
| MS10 | Counterfactual validity (section 7); PPO comparison (section 3) if Stretch time allows | Relation pass/fail matrix; PPO versus DP table if done |
| MS11 | Test-set run and final report | Held-out results with deflated Sharpe; test-set lock audit passes |
| Stretch | Pipeline and network (section 10) | Tick-to-trade and per-backend tables; logged pipeline inputs replay to identical orders in batch |

## Scope tiers

Core and Strong come to roughly 1,000 hours, about a year at 20 hours per week. Every
item is tiered; the timeline covers Core and Strong, and Stretch items, the live
pipeline among them, follow MS11 or are taken when a milestone finishes early.

| Tier | Items |
|------|-------|
| Core | Data layer, decoder, tick-indexed book with `std::map` baseline, matching engine, scheduler, fork, registry; signals from book state, queue dynamics and trade flow with ridge; fill model and markouts; Avellaneda-Stoikov and DP quoting; backtest with attribution; regime forecast methods A and B; invariants, differential, property and determinism tests; hosted CI |
| Strong | Cross-asset signals, LightGBM, online RLS; competing-risks fill model and queue value; quoting extensions; impact section; queue-reactive simulator and method C; event transformer with float32 AVX2 kernel; relation harness without shrinking on replay, the queue-reactive simulator and the event transformer; fuzzing, sanitizers, self-hosted CI |
| Stretch | Pipeline mode (section 10): feed publisher and handler, all six receive paths, line arbitration and gap recovery, SoupBinTCP and OUCH; int8 and AVX-VNNI kernels, ONNX Runtime and LibTorch baselines; PPO; M3 and Linna et al. reproduction in section 7; full relation harness with shrinking; GenMC and TLA+ checks; mutation testing; BOLT; B-tree and sorted vector books |

## Timeline

Assumes about 20 hours per week starting mid-October 2026, covering Core and Strong
items; rescaled if that changes. The regime forecast follows the backtest directly
because it is the only deliverable with a hard deadline: it must be frozen before the
November 2027 compliance date. If MS7 slips past August 2027, method C is left out of
the frozen bundle and methods A and B are frozen alone.

| Milestone | Weeks | Cumulative | Finish |
|-----------|-------|------------|--------|
| MS0 Toolchain and CI | 2 | 2 | Oct 2026 |
| MS1 Data layer and decoder | 4 | 6 | Nov 2026 |
| MS2 Order book study | 3 | 9 | Dec 2026 |
| MS3 Engine, registry, RNG | 5 | 14 | Jan 2027 |
| MS4 Features and signals | 5 | 19 | Feb 2027 |
| MS5 Fills | 3 | 22 | Mar 2027 |
| MS6 Quoting and backtest | 5 | 27 | Apr 2027 |
| MS7 Simulator and regime forecast | 6 | 33 | Jun 2027 |
| MS8 Impact and execution | 4 | 37 | Jul 2027 |
| MS9 Transformer and kernel | 5 | 42 | Aug 2027 |
| MS10 Counterfactual validity | 4 | 46 | Sep 2027 |
| MS11 Test-set run and report | 3 | 49 | Sep 2027 |

Checkpoints where the project stands on its own:
- **After MS3**: ITCH decoder and data store, order book study and matching engine with
  benchmarks; a complete systems project.
- **After MS6**: full market-making strategy with attribution; a complete quant project.
- **After MS7**: pre-registered forecast published, about five months before the
  November 2027 compliance date.

## Deliverables

| Deliverable | Content |
|-------------|---------|
| Repository | Code, tests, CI, reproduction commands |
| README | Headline results, latency and benchmark tables generated from the registry, caveats |
| Milestone reports | Static HTML per milestone with tables, plots and configs |
| Benchmark suite | Order book study, inference kernel, build optimisations; receive paths if the Stretch pipeline is built |
| Pre-registration | Frozen forecast bundle with hashes, published before the compliance date |
| Technical report | Paper-style write-up of Q1, Q2, E1 and E2, suitable for arXiv |
| Architecture records | `DESIGN.md` and decision records in `docs/adr/` |
| Blog posts | One per major result, for a non-specialist audience |

## Environment

WSL2 Ubuntu 24.04 only (no native Linux), GCC 13+ or Clang 17+, C++23 (for
`std::byteswap`), CMake + Ninja, Google Benchmark, GoogleTest, RapidCheck, libFuzzer,
nanobind, Apache Arrow, zstd, zlib-ng or ISA-L, SQLite, toml++, `perf`, Docker. Python
with NumPy, Polars, statsmodels, lifelines, LightGBM, PyTorch with CUDA on the RTX 4060,
Gymnasium, matplotlib. Ubuntu's `linux-tools` packages do not match the WSL2 kernel, so
`perf` is built from Microsoft's WSL2 kernel source (`tools/perf`).

## Layout

```
hft/
  CMakeLists.txt
  src/
    data/       ingest, zstd store, index, checkpoints, reference data
    feed/       ITCH decoder, MoldUDP64, receive backends
    book/       L3 book and comparison implementations
    engine/     matching engine, scheduler, snapshot
    sources/    replay, agent and external event sources
    strategy/   features, models, inference kernel, policy
    risk/       pre-trade checks
    gateway/    in-process and OUCH gateways, exchange simulator
    core/       ring buffers, pools, RNG, timing
  bindings/     nanobind module and Gym environment
  apps/         ingest, replay, pipeline, exchange, labeler, sweep
  bench/        benchmarks
  tests/        unit, property, fuzz, relation harness
  configs/      experiment configs
  research/     training, statistics, analysis
  docker/
```

## References

- Schaurecker et al., Event History Over Scale: Compact Transformers for Low-Latency LOB Forecasting, arXiv 2610.02917 (Oct 2026)
- Linna et al., Repurposing Deep LOB Forecasting for Scenario-Conditioned Market Impact, arXiv 2609.16930 (Sep 2026)
- M3: State-Event Generative Foundation Model for Market Microstructure, arXiv 2608.19227 (Jul 2026)
- Noble, Rosenbaum, Souilmi, Bridging the Reality Gap in LOB Simulation, arXiv 2603.24137 (Mar 2026)
- DiffLOB: Diffusion Models for Counterfactual Generation in LOBs, arXiv 2602.03776 (Feb 2026)
- LOBERT: Foundation Model for LOB Messages, arXiv 2511.12563 (Nov 2025)
- LOB-Bench: Benchmarking Generative AI for Finance, arXiv 2502.09172 (2025)
- Stoikov, The Micro-Price, Quantitative Finance (2018)
- Cont, Kukanov, Stoikov, The Price Impact of Order Book Events (2014)
- Xu, Gould, Howison, Multi-Level Order-Flow Imbalance in a Limit Order Book, Market Microstructure and Liquidity (2018)
- Moallemi, Yuan, A Model for Queue Position Valuation in a Limit Order Book (2016)
- Cont, de Larrard, Price Dynamics in a Markovian Limit Order Market (2013)
- Huang, Lehalle, Rosenbaum, Simulating and Analyzing Order Book Data: The Queue-Reactive Model (2015)
- Guéant, Lehalle, Fernandez-Tapia, Dealing with the Inventory Risk (2013)
- Guilbaud, Pham, Optimal High-Frequency Trading with Limit and Market Orders (2013)
- Cartea, Jaimungal, Ricci, Buy Low, Sell High: A High Frequency Trading Perspective (2014)
- Avellaneda, Stoikov, High-Frequency Trading in a Limit Order Book (2008)
- Dayri, Rosenbaum, Large Tick Assets: Implicit Spread and Optimal Tick Size, Market Microstructure and Liquidity (2015)
- Huang, Lehalle, Rosenbaum, How to Predict the Consequences of a Tick Value Change? Evidence from the Tokyo Stock Exchange Pilot Program, Market Microstructure and Liquidity (2016)
- SEC Tick Size Pilot Program (2016 to 2018) and its published assessments
- Bouchaud, Gefen, Potters, Wyart, Fluctuations and Response in Financial Markets (2004)
- Lillo, Mike, Farmer, Theory for Long Memory in Supply and Demand (2005)
- Tóth et al., Anomalous Price Impact and the Critical Nature of Liquidity (2011)
- Donier, Bonart, Mastromatteo, Bouchaud, A Fully Consistent, Minimal Model for Non-Linear Market Impact (2015)
- Gatheral, No-Dynamic-Arbitrage and Market Impact (2010)
- Gatheral, Schied, Slynko, Transient Linear Price Impact and Fredholm Integral Equations, Mathematical Finance (2012)
- Maitrier, Loeper, Bouchaud, Generating Realistic Metaorders from Public Data, arXiv 2503.18199 (2025)
- Obizhaeva, Wang, Optimal Trading Strategy and Supply/Demand Dynamics (2013)
- Almgren, Chriss, Optimal Execution of Portfolio Transactions (2001)
- Bailey, López de Prado, The Deflated Sharpe Ratio (2014)
- SEC press release 2026-54, proposed rescission of Rules 611 and 610(e) (Jun 2026)
- Nasdaq TotalView-ITCH 5.0, MoldUDP64 and OUCH 5.0 specifications
