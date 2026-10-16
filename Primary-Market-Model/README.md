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
