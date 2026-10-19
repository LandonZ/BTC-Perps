# BTC basis theo and historical backtest

The current strategy buys a discounted perp against a spot short and sells a
premium perp against a spot long. Define
`basis_bps = 10,000 * (perp / spot - 1)`; a perp 10 bps below spot has a basis of
**-10 bps**, or a positive 10 bps discount.

## Current rule

- Buy basis: enter **long perp / short spot** when basis is **below -5 bps**.
- Sell basis: enter **short perp / long spot** when basis is **above +5 bps**.
- Both entries require positive projected profit for that side.
- Forecast the exit at **-5 bps for longs / +5 bps for shorts**, with a
  **five-minute maximum** holding period.
- Close at the first observed positive total PnL, when a long basis reaches
  -5 bps or higher or a short basis reaches +5 bps or lower, or at the
  five-minute deadline, whichever comes first.
- Hold one equal-BTC pair at a time. Do not close and reopen in the same minute.
- Fees, spreads, slippage, borrowing, and financing default to **zero**. Funding
  remains included. Default quantity is 0.01 BTC per leg.

A profitable exit can happen before the -5 bps target is reached. For example,
buying at -10 and exiting at -7 earns 3 bps from the basis when spot is unchanged,
before funding. The target is an assumption, not a guarantee of convergence.

## Run

From the repository root:

```bash
make -C Primary-Market-Model theo
make -C Primary-Market-Model test
make -C Primary-Market-Model backtest
```

`theo` runs a **synthetic five-minute scenario**, not live data. `backtest` uses
retained/reacquired historical Coinbase minute candles and hourly funding. Its
outputs are `Primary-Market-Model/results/basis-both-5m/{summary.json,trades.csv,equity.csv}`.

Regenerate the minute price history if needed:

```bash
python3 Primary-Market-Model/tools/download_basis_data.py \
  --start 2026-06-03 --end 2026-10-01
```

Plot the completed results with Python/matplotlib:

```bash
python3 Primary-Market-Model/tools/plot_backtest.py \
  Primary-Market-Model/results/basis-both-5m
```

The plotter writes `pnl.png`, `pnl.svg`, and `report.md`. If matplotlib is installed
in a separate directory, provide `--python-packages DIRECTORY`. Matplotlib is
used only for reporting; the pricer and backtest are C++17.

To disable early profit-taking and wait for convergence or timeout:

```bash
make -C Primary-Market-Model backtest \
  BACKTEST_ARGS="--profit-exit 0 --output results/basis-both-5m-target"
```

The default is **early profit-taking enabled**. This alternative is an optional
sensitivity check, not an optimized result or the main strategy.

## Funding and theo

The pricer implements the hourly rule supplied in the reference image:

```text
mark = median(perp_bid, perp_ask, last_trade)
B = 1-hour TWAP of 10,000 * (mark / index - 1)
F_next_bps = 0.75 * (B / 24) + 0.25 * F_previous_bps
long_funding_paid_per_BTC = sum(projected_mark * F_hourly_bps / 10,000)
expected_exit_perp_mid = spot_reference * (1 + exit_basis_bps / 10,000)
theo = expected_exit_perp_mid - long_funding_paid_per_BTC
```

The one-hour TWAP gap is supplied **before** division by 24. Future TWAP and
funding mark are held at the supplied levels in this scenario forecast. Positive
funding is paid by long perps; negative funding is received. Only settlement
boundaries in `(entry, exit]` count. Five-minute holds often cross no hourly
boundary, but a hold entering two minutes before one can pay or receive funding.

With constant B, funding approaches B/24. This formula has a steady
funding-neutral gap of **zero**, not -5 bps. The **-5 bps trading threshold is a
strategy choice**, not an equilibrium implied by this funding rule.

The formula matches the [original INTX rules](https://www.coinbase.com/international-exchange/legal/trading-rules).
The upgraded international product has continuous funding; the project uses the
**supplied historical hourly assumption** and the corresponding retained history.
[Coinbase migration details](https://help.coinbase.com/en/international-exchange/deribit/coinbase-faqs).

Example valuation:

```bash
Primary-Market-Model/build/theo-pricer \
  --spot 100000 --perp 99900 --index 100000 \
  --twap-basis-bps -10 --previous-funding-bps -0.05 \
  --horizon-minutes 5 --minutes-to-funding 60 --exit-basis-bps -5
```

No funding boundary is crossed. Expected exit perp price and theo are 99,950.
The conditional long-basis profit is 5 bps, or 0.50 at 0.01 BTC. This is a
scenario calculation, not evidence of an actual fill or future convergence.
For a perp above spot, the scenario pricer defaults its target to +5 bps and
selects the short-perp pair only when basis exceeds +5 bps with positive edge.

## Coinbase US BIP funding (live venue)

INTX BTC-PERP has been paused since 2026-10-01, so live/paper trading uses the
Coinbase Derivatives nano BTC perp-style future `BIP-20DEC30-CDE` (0.01 BTC,
$5 tick, hourly funding) hedged with `BTC-USD` spot. `model/bip-funding.{h,cpp}`
implements Coinbase's published rule:

```text
futures mark = 3m VWAP | else 3m TWAP of mid | else spot mark + previous gap
spot mark    = 3m VWAP | else 3m TWAP of mid | else futures mark + previous gap
Premium      = mean over the hour's 20 samples of (futures mark - spot mark) / spot mark / 24
Rate         = 0.75 * Premium + 0.25 * Previous rate     (no published cap)
Long pays    = contracts * 0.01 * mark * Rate            (negative rate: long receives)
```

This is the same recurrence as `next_funding_bps` above. Only the mark inputs
(3-minute VWAP samples instead of a median mark) and the per-contract payment
differ. `FundingEstimator` builds the marks and projected rate from streamed
trades and quotes for the paper trader. It publishes no rate for an hour with
no futures data, and none for a partial first hour.

Funding accrues hourly but is cash-settled at the midday and end-of-day margin
runs. Coinbase's "examples" page shows 0.010% on $1,000 notional as $1.00; the
correct product is $0.10. The funding-rate page's 0.1% -> $1.00 example is the
one the tests use. Open questions: whether "spot" means Coinbase BTC-USD or the
`index_price` in the product API, and where to get historical BIP funding
(Coinbase says "available upon request").

## Live recording and paper trading (BIP vs BTC-USD)

One C++ engine (`paper/paper-engine.{h,cpp}`) runs recorded ticks, Allen's minute
candles, and a live stream, so the backtest and the paper trader use the same
decision and fill code.

1. Record (on a laptop; the cloud sandbox cannot reach Coinbase). Needs `pip install websockets`:

   ```bash
   python3 Primary-Market-Model/tools/record_coinbase.py              # Ctrl-C to stop
   ```

   This writes `data/live/YYYY-MM-DD/events.csv` (top-of-book changes `Q` and trades `T`
   for both legs, from the public level2 and market_trades channels) and `funding.csv`
   (a REST poll every 15 s of the BIP product: `funding_rate`, `funding_time`, `index_price`).
   Compare the poll with `FundingEstimator` to settle the open questions above.

2. Replay the recording, with one run per latency (best, middle and worst case):

   ```bash
   cd Primary-Market-Model && make build/paper-trader
   ./build/paper-trader --events ../data/live/2026-10-15/events.csv --latency-ms 5,50,250 \
       --perp-fee-usd 0.15 --spot-taker-bps 6       # placeholder fees: use our real tier
   ```

3. Paper trade live: `python3 tools/record_coinbase.py --stdout | ./build/paper-trader --live`.
   Results are rewritten every minute and a status line is printed to stderr.

4. Minute candles: `./build/paper-trader --candles` (same files as `basis-backtester`, bid = ask
   = open, latency 0). Taker mode should come close to the zero-fee `basis-backtester` result,
   and it lets fees be added. Funding here uses the BIP rule, not the recorded INTX rates.

Modes:

- `--mode taker` (default) is Allen's basis rule on **executable** prices. It sells the basis when
  `perp_bid / spot_ask - 1 > center + threshold` and buys when `perp_ask / spot_bid - 1 < center - threshold`.
  It exits on the first profit net of fees, convergence back to the band, or `--hold-minutes`.
- `--mode maker` is the naive market maker, quoting with Allen's `theo::price()`. Theo is the spot mid
  shifted by the fair basis and the projected BIP funding over `--horizon-min`. The half-width is
  `(two perp maker fees + --profit-bps) / 2`, with `--skew-bps-per-btc` inventory skew, a
  `--max-inventory` cap and the $5 tick. Quotes never cross the book. It does not hedge yet;
  cop and lean comes after the Thursday meeting. The ±5 bp taker band comes from Binance's
  funding kink; BIP's funding has no such kink, so the maker does not use it.
- The fair basis defaults to a causal EMA of the perp-vs-spot mid basis (`--ema-halflife-min 10`).
  Nothing trades during the first half-life. BIP trades about 2–3 bp rich to BTC-USD, and with a
  fixed 0 center the maker sold the perp below market until it hit the inventory cap.
  `--ema-halflife-min 0 --center-bps X` restores a fixed center. Candle runs default to the fixed
  center so they stay comparable to `basis-backtester`.
- Default rows are latency 0 (idealized), 5, 50 and 250 ms. Maker skew defaults to 100 bp per BTC
  (1 bp per contract); Allen's default of 4 moves the quotes by less than one $5 tick.
- `--split 0.5` runs the first half of the recording (TRAIN) and the rest (TEST) as separate
  backtests. Choose parameters on TRAIN and report TEST.
- If both feeds go silent for over 10 s (`--max-gap-ms`), for example while the laptop is asleep,
  the engine treats it as a disconnect: it pulls all quotes and in-flight entries and waits for
  fresh books. The `gaps` column counts these.

Fill model:

- An order decided at time t reaches the exchange at t + latency. A taker order fills at the
  top of book in force at that moment. `signal_basis_bps` vs `entry_basis_bps` in `trades.csv`
  is the latency slippage.
- A maker quote (or its replacement or cancel) goes live after the latency. It fills only when a
  perp trade prints **through** its price (`--fill-at-touch 1` also counts prints at the price),
  for the full quote size. That is optimistic about queue position and size.
- Time is our receive clock (`recv_ms`). Events sharing a timestamp are applied together before
  deciding. `--from-ms/--to-ms` select train/test windows.
- Fees default to zero and the CLI warns about it. Set them before reading anything into PnL.

Outputs per latency go to `results/paper/latency-<N>ms/`: `trades.csv` (Allen's columns plus
fees and signal basis), `fills.csv`, `equity.csv` and `summary.json`.

## Historical accounting

At each available minute open, the backtester evaluates the entry/exit rules.
The preceding 60 **completed** minute closes proxy the funding premium TWAP;
the latest funding timestamp at or before entry supplies the prior rate. An
incomplete trailing hour skips that entry check. No future exit price or
funding rate is used to select the entry.

The forecast uses the supplied recurrence. **Realized funding PnL uses actual
recorded rates and funding marks**, not the forecast. Equal BTC quantities use
these exact cash-flow formulas as spot and perp prices change:

```text
price_PnL = direction * q * [(perp_exit - perp_entry) - (spot_exit - spot_entry)]
funding_PnL = -direction * q * sum(recorded_mark * recorded_rate)
net_PnL = price_PnL + funding_PnL
trade_PnL_bps = 10,000 * net_PnL / (q * entry_spot)
```

Direction is +1 for buy basis and -1 for sell basis. Positive funding is a
long-perp debit and short-perp receipt; negative funding reverses the signs.

The equity CSV includes realized PnL plus mark-to-market changes while a pair
is open. Drawdown is measured at matched minute opens. Trade bps are normalized
to each entry's spot notional; their average is not an account return.

## Data limits

The historical prices are Coinbase BTC-USD spot and BTC-PERP-INTX perp candles,
from the [public candle API](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/public/get-public-product-candles).
USD and USDC are assumed at parity. Minute trade opens are idealized simultaneous
fills, not actual executable books. The TWAP proxy is not the venue's official
per-second mark/index series. Funding timestamps are assumed to be settlement
and publication times, with no latency.

Minute data checks profitability only at observed minute prices. It cannot
verify the first profitable second, resting maker orders, queue position, or
intra-minute drawdown. Missing prices are not interpolated. If a deadline quote
is missing, the entire trade is excluded from PnL/equity and counted explicitly;
the resulting sample is conditional on measurable trades. The final pair can
close early at the end of the dataset and is flagged.

No borrow availability, partial fills, currency hedge, funding publication
latency, financing, or liquidation is simulated. A positive zero-cost backtest
is not proof of executable arbitrage. See [data inventory](../data/README.md).

## Source files

- `model/theo-model.{h,cpp}`: funding and paired valuation.
- `model/theo-pricer.cpp`: scenario CLI, targeting the respective side's threshold.
- `backtester/basis-backtest.{h,cpp}`: chronological historical simulator.
- `backtester/backtest-cli.cpp`: backtest CLI.
- `tests/backtest-tests.cpp`: causal entry checks, both cash-flow signs,
  profit/convergence exits, strict time cap, missing data, and equity reconciliation.

The older directional regression sources and original committed files remain
preserved. They are inactive in this build. The default is `--strategy both-basis`. To run just one side:

```bash
make -C Primary-Market-Model backtest \
  BACKTEST_ARGS="--strategy long-basis --output results/basis-long-5m"
make -C Primary-Market-Model backtest \
  BACKTEST_ARGS="--strategy short-basis --output results/basis-short-5m"
```

The earlier 24-hour carry engine remains available with `--strategy theo-carry`
for comparison; it is not the default. The earlier long-only report is preserved
under `results/basis-5m/`. Combined-run side totals share a single position limit;
they need not equal independent side-only runs when opportunities overlap.
