# Legacy cost-budget quote rule

This document preserves the earlier cost-budget model. It is no longer the
default. To reproduce its original settings, explicitly pass `--quote-rule
cost-budget --risk-buffer-bps 1 --profit-bps 1 --skew-bps 1 --latency-ms 50
--reprice-ms 1000` with the original minute-close event file. See
[BINANCE-MAKER.md](BINANCE-MAKER.md) for the current spot-fee rule.

# Binance BTCUSDT market maker

The active `make backtest` target replays Binance USD-M BTCUSDT perpetual data.
It quotes the perp around Binance BTCUSDT spot, subject to low absolute funding,
both maker fees, a funding reserve, a risk buffer, and a profit target. Orders
are simulated only. The Coinbase paper engine and historical basis backtest
remain available separately.

The market-making band is **-5 to +5 bps of perp midpoint versus spot**, inclusive:
`basis_bps = 10000 * (perp_mid / spot - 1)`. Outside this band the engine withdraws
quotes, subject to exchange latency. Posted target prices must also stay within
`spot * (1 - 5/10000)` and `spot * (1 + 5/10000)`, rounded inward to valid ticks.
When clipping a quote to the band erases its required fee/risk/profit budget,
the model skips quoting. The band is configurable with `--min-basis-bps` and
`--max-basis-bps`. It is a strategy approximation using spot and midpoint, not an
exact reconstruction of Binance's averaged impact-premium damping region.

## Funding reference

Use the user-supplied [Binance funding-rate explanation](https://www.binance.com/en/support/faq/detail/360033525031)
as the reference for payment direction and timing:

- Funding cash flow to our position is `-signed_BTC * mark * rate`.
- Positive rates cost a long and credit a short; negative rates reverse this.
- The default schedule is 00:00, 08:00 and 16:00 UTC. Intervals can change, so
  replay reads `funding_interval_hours` from the archive rather than using the
  Coinbase hourly estimator. It charges positions held at recorded settlements.
- Funding is separate from exchange trading fees. The model charges neither a
  maker fee on funding nor prorated funding for a hold crossing no settlement.

The page also describes impact-depth premium, time weighting, interest, the
damper and caps. It does **not** define funding as the simple perp/spot midpoint
gap. In bps, its uncapped rate is
`(weighted_premium + clamp(interest_8h - weighted_premium, -5, +5)) * interval_hours/8`,
then the applicable contract floor/cap is applied. Interest is ordinarily 1 bp
per eight hours. These are reference rules, not a new forecast implemented from
the bookTicker feed: top of book lacks the required impact-depth series.

For decisions, this implementation uses the **last settled historical rate** as
an explicit persistence proxy. It never supplies the next realized settlement
rate to an earlier decision. That proxy can differ from Binance's contemporaneous
projected rate. Actual recorded future rates affect realized funding only when
their settlement events arrive. The archival interval associated with the last
settlement is assumed to persist until another is observed; unexpected schedule
changes cannot be forecast from these files.

The FAQ mentions up to 15 seconds of funding transaction-time deviation. Replay
uses the archive's exact `calc_time`, without modeling that uncertainty. Its
settlement mark is the mark-price minute candle's **open** at that boundary, a
documented proxy for the official funding mark. The funding archive does not
contain the actual charged mark. The current FAQ is a semantic reference; this
replay does not retroactively recompute 2023/2024 rates under later rule changes.

## Quote bounds

For a static buy/sell cycle, the spread must cover:

```text
two maker fees + adverse inventory funding + risk buffer + desired profit
```

Defaults use a **2 bps maker fee on each fill** (0.02%, no BNB discount), as in
[Binance's fee calculation example](https://www.binance.com/en/support/faq/detail/360033544231).
This is a configurable scenario assumption; the public fee table did not expose
an account-specific tier. A 2 bps buy and 2 bps sell cost about 4 bps together.
With the 1 bp risk buffer and 1 bp profit target, the starting full quote width
is approximately 6 bps before tick rounding and passive-book adjustments.

Let `R` be spot adjusted by inventory skew, `r` the fee as a fraction per fill,
`f` the flat fee per BTC per fill, and `C` the funding + risk + profit budget in
USDT per BTC. The exact fee-aware endpoints before tick/book adjustments are:

```text
maximum bid = (R - f - C/2) / (1 + r)
minimum ask = (R + f + C/2) / (1 - r)
cycle net   = ask*(1-r) - bid*(1+r) - 2*f - funding_reserve - risk_buffer
```

Bid rounds down and ask rounds up to the tick. Passive guards can widen them.
`minimum_spread_bps` repeats the calculation with zero desired profit and is
the cost floor around that reservation price, rounded to ticks. A zero-cost
market still requires at least one tick between quotes. A requested quote
below its cost floor is never submitted.

Fees imply a lower bound, but there is no universal economic upper bound on
spread. `--max-spread-bps` is a policy ceiling on the **full** width; if the
needed quote is wider, the model skips it. Wider quotes need not attract fills.
The default basis band is tighter: its two endpoints permit at most a 10 bps
full spread. Both the basis-band constraint and maximum-width constraint apply.
Funding cost reserves the adverse direction for either long or short inventory,
using full payments crossed by the assumed horizon. Funding received is not
counted as extra guaranteed profit.

These formulas budget a cycle at one decision. Repricing, different fill times,
spot moves, adverse selection and unmatched inventory can make actual PnL
negative even when both initial quote endpoints satisfy the budget.

## Download and run

From the repository root:

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py
make -C Primary-Market-Model test
make -C Primary-Market-Model backtest
```

The default prepares **2023-07-20 00:00–01:00 UTC**, a small low-funding replay
sample, not a representative profitability study. Full daily ZIPs are retained
with SHA-256 verification, and the prepared window is configurable:

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py \
  --date 2023-07-20 --start-hour 7 --hours 2 \
  --output data/binance/maker/2023-07-20/events-07-09.csv
make -C Primary-Market-Model maker-backtest \
  MAKER_ARGS="--events ../data/binance/maker/2023-07-20/events-07-09.csv --output results/binance-maker-07-09"
```

Preparation now uses a stable external merge sort with bounded RAM and file handles, including full-day windows. Recent
bookTicker archives are unavailable: a [Binance repository maintainer confirmed
that publication stopped](https://github.com/binance/binance-public-data/issues/334).
The futures REST API also returned HTTP 451 from this environment. The downloader
uses public archive files without authentication. It does not silently fabricate
books for dates where the bookTicker ZIP is missing.

Data sources are perp bookTicker, perp aggTrades, spot minute klines, mark-price
minute klines, and monthly fundingRate. Spot becomes available only at a
completed candle's `close_time + 1ms`. It is a causal but coarse spot reference,
with up to roughly a minute of lag. Perp book and trade data remain tick-level.
The manifest records exact URLs, checksums, counts, and approximations.

## Configurable starting assumptions

| Setting | Default | Meaning |
|---|---:|---|
| `--maker-fee-bps` | 2 | Fee per executed fill |
| `--max-funding-bps` | 1 | Maximum absolute funding bps **per settlement**, not hourly |
| `--min-basis-bps` / `--max-basis-bps` | -5 / +5 | Perp midpoint basis gate and posted quote price bounds |
| `--horizon-minutes` | 5 | Funding cost horizon; no enforced inventory exit |
| `--risk-buffer-bps` | 1 | Full-cycle extra cost budget |
| `--profit-bps` | 1 | Full-cycle conditional target |
| `--max-spread-bps` | 20 | Maximum full quoted width |
| `--order-btc` | 0.01 | Size per side |
| `--max-inventory-btc` | 0.05 | Absolute inventory cap |
| `--skew-bps` | 1 | Reservation shift per 0.01 BTC against inventory |
| `--latency-ms` | 50 | Decision-to-exchange latency |
| `--reprice-ms` | 1000 | Minimum ordinary reprice interval |
| `--participation` | 0.1 | Maximum share of an eligible historical trade |
| `--fill-at-touch` | 0 | Require trade through quote by default |
| `--tick` | 0.1 | Assumed USDT price increment |
| `--quantity-step` | 0.001 | Assumed BTC size increment |
| `--min-notional` | 100 | Conservative assumed minimum order value |

The fee, trading filters and policy thresholds are assumptions, not verified
historical account settings or optimized parameters. Replace them for another
fee tier or historical regime. Low funding requires `abs(last_settled_bps) <= cutoff`.
Missing funding, stale prices, excessive spread or invalid passive quotes stop
new quoting. A funding cutoff crossing also cancels remaining quotes; it does
not erase or liquidate inventory already held.

## Pretend fills and accounting

Orders rest until a later historical trade reaches them. They are not filled
merely because a candle contains the price or because we decided to quote.

- Quote changes/cancels arrive after latency; the prior quote remains exposed.
  A post-only order crossing the arrival book is rejected (cancel-then-post).
- At identical timestamps, events are batched before new decisions. A quote
  cannot fill at or before its arrival timestamp.
- Seller-aggressed trades (`buyer_maker=true`) can fill our bid; buyer-aggressed
  trades can fill our ask. Default fills require a print **through** the limit.
- Fill size is the minimum of remaining quote size, trade size × participation,
  and remaining inventory capacity, rounded down to the quantity increment.
  This allows partial fills. It approximates execution without a queue model.
- Filled prices are our resting limits, not the historical print prices. Fees
  use actual filled notional. Funding uses actual held inventory at settlement.
- `net = trade_cash + inventory*perp_mid + funding_cash - trading_fees`.
  Final inventory stays open and marked at mid; no fictitious final maker fill
  or taker liquidation is added. Spot is only a reference; there is no hedge.

Outputs in `results/binance-maker/` are `quotes.csv` (bounds, cost budgets and skip
reasons), `fills.csv`, `equity.csv`, and `summary.json`. A high-funding comparison
sample is also available locally for 2024-03-30 00:00–01:00 UTC; its default run
has zero fills because funding exceeds the cutoff.

The original reported samples were run before adding the +/-5 bps basis guard.
Use a separate output directory to compare the new rules against those baselines.

Main sources: `model/maker-model.{h,cpp}`, `backtester/maker-backtest.{h,cpp}`,
`backtester/maker-backtest-cli.cpp`, `tools/download_binance_maker.py`, and
`tests/maker-tests.cpp`.
