# References

Each entry: id, citation, what the project takes from it, where it applies. Only references that support a design decision.
Check: W = bibliographic details checked online 2026-10-10; K = standard reference, details from the publication itself.

## Domain: prediction markets

- RF1 W S. Dalen, "Toward Black Scholes for Prediction Markets: A Unified Kernel and Market Maker's Handbook", arXiv:2510.15205 (v2 2026-04-06). Logit jump-diffusion, belief volatility as a quoted risk factor. Applies: maker model (logit Avellaneda-Stoikov in pm/maker.hpp), variance estimate.
- RF2 W O. Saguillo, V. Ghafouri, L. Kiffer, G. Suarez-Tangil, "Unravelling the Probabilistic Forest: Arbitrage in Prediction Markets", arXiv:2508.03474 (2025). Market-rebalancing (within a market/condition) vs combinatorial arbitrage. Data: markets resolved 2024-04-01..2025-04-01, 17,218 conditions (8,659 single, 1,578 neg-risk markets), from executed trades (OrderFilled, PositionSplit, PositionsMerge), not the order book. Opportunity: |1 - VWAP sum| > 0.02, kept if profit >= 0.05 per dollar, no token above 0.95. Realised (no fees): 39.59M USD total; single-condition buy<1 5.90M, sell>1 4.68M; neg-risk buy NO 17.31M, buy YES 11.09M, sell YES 0.61M; combinatorial only ~0.09M. ~1% of election opportunities exploited; top account 2.01M over 4,049 txs, bot-like; non-atomic execution risk noted. No durations reported. Applies: arb scanner groups (pair and event), X2 default E_min 0.02 as reference point, X* executor, D10/D17; neg-risk buy-NO side (scanner covers it as bids-sum > 1 equivalent).
- RF3 W S. Dalen, "What Happens When Institutional Liquidity Enters Prediction Markets: Identification, Measurement, and a Synthetic Proof of Concept", arXiv:2604.10005 (v1 2026-04-11, v3 2026-07-02). WITHDRAWN by the author (to be superseded by an empirical paper). Research design only, synthetic simulation, no live-market figures. Applies: background only; do not cite as evidence.
- RF4 W K. P. Tsang, Z. Yang, "The Anatomy of a Blockchain Prediction Market: Polymarket in the 2024 U.S. Presidential Election", arXiv:2603.03136 (v1 2026-03-03, v3 2026-09-14). Minting/burning inside trades inflates naive volume (Oct Trump market: 391M USD turnover vs 958M naive); corrected price impact shows a shallower market (5 pp move cost 9.1M vs 15.6M naive); overstatement largest in thin, young markets (249 markets). Applies: volume/impact metrics must count mint/burn (F23); depth assumptions in D3.
- RF5 W B. Qin, R. Yang, "Polymarket-v1 Database", arXiv:2606.04217 (v2 2026-06-08). On-chain CTF Exchange trades 2022-11-21..2026-04-28, 1.20B trades, 1.30M markets, 61B USD nominal; aggressor side from settlement records; trade-classification methods near chance; distorts VPIN/OFI. Applies: F14; our feed gives taker side directly (last_trade_price side), no classification needed.
- RF27 W P. D. Dubach, "The Anatomy of a Decentralized Prediction Market: Microstructure Evidence from the Polymarket Order Book", arXiv:2604.24366 (v1 2026-04-27, v2 2026-05-14). 30B public-feed events over 52 days joined to on-chain trades; pre-registered panel of 600 markets. Facts: longshot spread premium; depth close to uniform; category-dependent effective spreads; median feed ingestion delay < 50 ms with a multi-second tail; wash-trade share median 1% (tail 22%). Trade direction from the public feed matches on-chain truth in ~59% of buckets (mean 0.615, CI 0.58-0.65; Nasdaq Lee-Ready ~80%); effective half-spread sign flips in 50-67% of markets. Applies: D21 (fills by price, not feed side); V3; caution on any side-based metric.
- RF6 K R. Hanson, "Combinatorial Information Market Design", Information Systems Frontiers 5(1), 2003 (LMSR). Applies: background on automated market makers vs CLOB.
- RF7 K J. Wolfers, E. Zitzewitz, "Prediction Markets", Journal of Economic Perspectives 18(2), 2004. Applies: background; prices as probabilities.
- RF8 W Polymarket documentation (docs.polymarket.com), read 2026-10-10: orders, fees, rate limits, order lifecycle, CTF split/merge/redeem, negative risk, resolution. Applies: VENUE.md F1-F26.

## Market making and microstructure

- RF9 K M. Avellaneda, S. Stoikov, "High-frequency trading in a limit order book", Quantitative Finance 8(3), 2008. Reservation price and optimal spread. Applies: pm/maker.hpp, ITCH backtest strategy.
- RF10 K O. Gueant, C.-A. Lehalle, J. Fernandez-Tapia, "Dealing with the inventory risk: a solution to the market making problem", Mathematics and Financial Economics 7(4), 2013. Inventory-bounded closed-form quotes. Applies: max_inv handling, ITCH GLFT strategy.
- RF11 K A. Cartea, S. Jaimungal, J. Penalva, "Algorithmic and High-Frequency Trading", Cambridge University Press, 2015. Inventory, adverse selection, execution. Applies: risk R9-R11, ledger marks.
- RF12 W W. Huang, C.-A. Lehalle, M. Rosenbaum, "Simulating and analyzing order book data: The queue-reactive model", JASA 110(509), 2015, 107-122, doi:10.1080/01621459.2014.982278. Applies: queue dynamics behind V3 maker queue estimate; src/sources/queue_reactive.hpp.
- RF13 W C. C. Moallemi, K. Yuan, "A model for queue position valuation in a limit order book", working paper, Columbia, 2016 (rev. 2017). Value of queue position = spread vs adverse selection. Applies: why V3 is conservative (no credit for cancels ahead).
- RF14 K L. Glosten, P. Milgrom, "Bid, ask and transaction prices in a specialist market with heterogeneously informed traders", Journal of Financial Economics 14(1), 1985. Applies: adverse selection; stated SimVenue limit.
- RF15 K A. Kyle, "Continuous Auctions and Insider Trading", Econometrica 53(6), 1985. Applies: price impact; stated SimVenue limit (others do not react to us).
- RF16 K L. Harris, "Trading and Exchanges: Market Microstructure for Practitioners", Oxford University Press, 2003. Applies: order types, priority, fill rules vocabulary.
- RF17 K FIX Trading Community, FIX 4.4 / 5.0 specification: OrdStatus (New, PartiallyFilled, Filled, Canceled, PendingCancel, Rejected, PendingNew, Expired) and ExecType. Applies: OMS states S* map onto FIX states; Unknown is our addition for timeouts.

## Systems and performance

- RF18 W G. Langdale, D. Lemire, "Parsing Gigabytes of JSON per Second", The VLDB Journal 28(6), 2019, doi:10.1007/s00778-019-00578-5. SIMD structural indexing. Applies: net/tape.hpp stage 1; single-pass decoder in pm/msg.hpp.
- RF19 K M. Thompson, D. Farley, M. Barker, P. Gee, A. Stewart, "Disruptor: High performance alternative to bounded queues for exchanging data between concurrent threads", LMAX technical paper, 2011. Single-writer ring, cache-line padding, cached sequence. Applies: core/spsc.hpp (SpscRing, SpscBytes).
- RF20 K U. Drepper, "What Every Programmer Should Know About Memory", 2007. Cache lines, TLB/huge pages, prefetch. Applies: core/pool.hpp, book/tick_book.hpp layout.
- RF21 K Intel 64 and IA-32 Architectures Optimization Reference Manual. Applies: AVX2/BMI2 use, rdtsc/rdtscp fencing (core/tsc.hpp).
- RF22 K J. K. Salmon, M. A. Moraes, R. O. Dror, D. E. Shaw, "Parallel random numbers: as easy as 1, 2, 3", SC '11. Counter-based RNG (Philox). Applies: core/philox.hpp, SimVenue fault draws.

## Testing and verification

- RF23 W W. Wilson, "Testing Distributed Systems w/ Deterministic Simulation", Strange Loop 2014 (FoundationDB). Single-threaded simulation, simulated external I/O, determinism. Applies: A4 replay, SimVenue inline (D7), seeded faults.
- RF24 K K. Claessen, J. Hughes, "QuickCheck: A Lightweight Tool for Random Testing of Haskell Programs", ICFP 2000. Applies: RapidCheck property tests vs reference models (OMS, book, decoder).
- RF25 K LLVM libFuzzer documentation. Applies: fuzz targets (itch, tape, oms).
## To read before citing specifics

RF1 (calibration pipeline details), RF27 (how feed trade side was defined: confirm it is the `side` field of last_trade_price).
