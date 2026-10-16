# Market data

The active paired basis model makes passive **Binance BTCUSDT perp** entries
and immediately crosses spot for equal BTC in the opposite direction. Download checksum-verified perp book updates, perp and spot aggregate
trades, mark-price minute candles and historical funding with:

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py
make -C Primary-Market-Model backtest
```

The default prepares 2023-07-20 00:00–01:00 UTC from full daily archives under
`data/binance/maker/2023-07-20/archives/`. `events-spot-trades.csv` contains the replay window;
`events-spot-trades.manifest.json` records sources, SHA-256 hashes and approximations. A
2024-03-30 one-hour high-funding comparison is also retained locally. Recent
bookTicker archives are unavailable; no book data is synthesized. See the
[paired basis documentation](../Primary-Market-Model/BASIS-MAKER.md).

Historical funding is recorded per settlement, with each archive row's interval.
Positive rates are a long debit and short credit. The current paired strategy uses fixed -5 bps equilibrium, cancels entry quotes
before funding, has no exits and assumes zero funding cash flows. The earlier
unhedged spot-fee comparison charged recorded funding. Spot references use the latest observed spot
trade as a midpoint proxy with an assumed crossing spread; perp fills use tick
trades. Legacy funding settlement marks use
the mark-price minute OPEN as a proxy. True historical spot bid/ask data was
unavailable from the checked public sources. The older minute-close stream and
last-settled-rate cost-budget strategy remain available as comparisons.

## Legacy Coinbase and other datasets

The legacy Coinbase scenario model is `Primary-Market-Model/model/theo-model.cpp`, run through
`Primary-Market-Model/build/theo-pricer`. It uses supplied scenario inputs and
**does not load historical CSVs or train a regression**. The new C++ backtester
uses the relevant historical files listed below. Trading and carrying
costs default to zero; the supplied hourly funding formula remains included.

## Relevant Coinbase data

- `coinbase/BTC-USD_spot_1m.csv`: historical spot minute OHLCV, restored for the backtest.
- `coinbase/BTC-PERP-INTX_1m.csv`: historical perp minute OHLCV, restored for the backtest.
- `coinbase/BTC-PERP-INTX_funding_1h.csv`: retained historical hourly funding and marks.

Minute price columns are `ts,open,high,low,close,volume`; timestamps are candle
start times. Completed closes become known one minute later. Spot USD and perp
USDC prices are assumed at parity. The backtester joins exact timestamps and
does not interpolate missing prices.

`coinbase/BTC-PERP-INTX_funding_1h.csv` contains historical Coinbase International
BTC perp funding and mark prices. It can provide a historical previous-funding
input for a matching timestamp; the pricer does not read it automatically.

Columns:

- `ts`: Unix timestamp in seconds.
- `funding_time`: UTC funding timestamp.
- `funding_rate`: fractional hourly rate; multiply by 10,000 for the CLI's bps.
- `mark_price`: funding mark price per BTC.

For example, `0.000005` is **0.05 bps**, not 5 bps. Historical realized funding
must not be used as information known before its publication.

The historical funding file alone cannot reconstruct the one-hour premium TWAP.
The model needs separately supplied prices, TWAP basis, previous funding,
holding period, time to next funding, and assumed exit basis. The current
scenario calculator uses the hourly formula supplied for this project; this
file is historical reference data, not confirmation of the current venue rules.

## Legacy files retained under the preservation instruction

These files existed in the last commit and are not inputs to the current theo:

| File | Purpose |
|---|---|
| `binance/BTCUSDT_perp_funding.csv` | Earlier Binance research |
| `kalshi/KXBTCPERP_funding.csv` | Earlier Kalshi research |
| `kalshi/KXBTCPERP_market_info.json` | Earlier Kalshi contract snapshot |
| `fetch_data.py` | Original multi-venue data collector |

They remain at their original paths because committed files must be preserved.
The original collector can recreate unrelated datasets. No historical data is
needed for the scenario pricer; the backtester needs the three Coinbase files.

## Cleanup

The Coinbase minute candles previously removed during scenario-only cleanup
were reacquired for the requested historical backtest. They are now relevant
inputs to the latest five-minute two-sided basis strategy. No committed dataset was removed.

Run the model from the repository root:

```bash
make -C Primary-Market-Model theo
```

Run the backtest with `make -C Primary-Market-Model basis-backtest`. Its PnL report
and chart are under `Primary-Market-Model/results/basis-both-5m/`.

Regenerate minute prices with:

```bash
python3 Primary-Market-Model/tools/download_basis_data.py --start 2026-06-03 --end 2026-10-01
```

See `Primary-Market-Model/README.md` for formulas, inputs, and assumptions.
