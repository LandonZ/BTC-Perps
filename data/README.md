# Market data

The active paired basis backtest uses **December 1, 2023 through February 29,
2024 UTC**, with equal-BTC spot hedges and continuous inventory across all 91 days.
The retained Binance archive folders are:

- `binance/maker/2023-12/`
- `binance/maker/2024-01/`
- `binance/maker/2024-02/`

Each month contains six checksum-verified ZIPs: perp `bookTicker`, perp and spot
`aggTrades`, mark-price minutes, funding settlements and premium-index minutes.
`month-inputs.manifest.json` records source URLs, SHA-256 hashes, sizes and
coverage checks. Premium-index candle CLOSEs become available at close_time + 1 ms.
Original funding timestamps, including millisecond offsets, are preserved.

The active strategy quotes the perp only outside a 1.5 bp band around the -5 bp
basis equilibrium. Maker fees are 0 bps; taker fees are 0.5 bps per fill. It
cancels entry quotes before funding and exits expected payers only outside the
flat funding region. Held positions pay or receive recorded funding. Spot
references use last trades as a midpoint proxy with an assumed 1 bp full spread.

`make -C Primary-Market-Model run` streams the archives into the backtest without
retaining expanded tick CSVs. Raw feeds are stably sorted in compact disk-backed chunks,
then merged exactly by transaction time. Every UTC day must contain spot trades,
perp trades and perp quotes. Detailed coverage and input provenance are retained
under `Primary-Market-Model/build/current/` with the current summary and equity.
See the [strategy and run documentation](../Primary-Market-Model/README.md).

Older Binance backtest data outside this window is removed after verification.
Kalshi folders and their data are excluded from cleanup.


## Other data

`data/kalshi/` and `Kalshi-Market-Model/` are preserved. They are excluded from
all backtest cleanup. The original multi-venue collector `data/fetch_data.py`
is retained as source code.

Unused Coinbase datasets, older Binance funding research, July 2023 replay
inputs and the March 2024 comparison are removed after the three-month result
passes validation. The active Binance data is limited to the three months above.
