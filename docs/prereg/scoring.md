# Scoring rules (fixed before any post-change data)

- Targets per treated stock: time-weighted quoted spread on Nasdaq and depth at the best,
  from apps/day_stats on the first post-compliance Nasdaq ITCH sample days.
- Conditioning: realized 1-minute volatility, traded notional and price on the scoring days are
  plugged into method A; changes are also scored as the difference between treated and
  untreated universe stocks.
- Errors: log error of the predicted change against the realized change per stock; coverage of
  the bands; CRPS of the combined forecast taken as uniform over its band.
- Delay: Nasdaq posts sample days irregularly; the forecast stays open until the first
  post-compliance sample day is available.
