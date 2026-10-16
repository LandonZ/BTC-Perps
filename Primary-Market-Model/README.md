# BTC basis strategy

Quote the Binance BTCUSDT perp bid below **−5 bps versus spot**, or its ask above
−5 bps. Every partial fill immediately gets an opposite **equal-BTC spot hedge**.
Quotes are 0.01 BTC; concurrent pairs and position size are uncapped by default.
**Execution fees are zero. Convergence never closes a position.**

## Funding-only exits

During the final **60 seconds before funding**, close both legs only for pairs
expected to **pay** funding, and only when the funding calculation is outside its
flat region. All other pairs remain open, including at the end of the replay.
Entry quotes pause during that final minute and resume after settlement.

Binance's eight-hour funding calculation is
`F = P + clamp(I - P, -5 bps, +5 bps)`, with the standard interest component
`I = 1 bp`. Therefore averaged premium **P from −4 to +6 bps** produces a flat
**1 bp** funding rate. Beyond those boundaries the rate scales linearly. Positive
estimated funding selects long-perp payers for exit; negative funding selects
short-perp payers. Receiving pairs stay open. The flat rate is low, but not zero.
See [Binance's funding explanation](https://www.binance.com/en/support/faq/detail/360033525031).

The exit signal uses a causal, linearly weighted prefix average of completed
**official premium-index 1-minute CLOSEs** within the funding period. A candle
becomes available at `close_time + 1 ms`. This approximates Binance's more
frequent premium sampling; it is not an exact historical funding forecast.
It is separate from the instantaneous perp/spot basis used for entries. Missing,
incomplete or stale premium history cannot trigger a funding exit. The upcoming
final settlement rate is never used early.

Held positions pay or receive the **recorded funding at settlement**. Funding
cash is realized immediately; open-position unrealized PnL contains price marks
and paid entry fees. Closed-pair lifetime PnL also attributes that pair's funding
without counting it twice in the portfolio. The funding mark is the settlement
minute's mark-price OPEN, a proxy for the official funding mark. July 2023 uses
00:00, 08:00 and 16:00 UTC; the engine follows archived interval updates.

## Run

From the repository root:

```bash
make -C Primary-Market-Model test
make -C Primary-Market-Model run
```

The default run uses the July 20, 2023 first-hour sample. Prepare missing data
with `make -C Primary-Market-Model sample`. The report runner downloads and
caches the month's premium-index feed when needed.

For a continuous month:

```bash
make -C Primary-Market-Model month MONTH=2023-07
```

The compact report is [results/result.html](results/result.html), with the
current July profit curve also exported as [results/result.png](results/result.png).
The HTML contains its own chart data. Temporary intermediate outputs are removed
by the report runner. A new run replaces the HTML; use
`ARGS="--output results/sample.html"` to retain another run.

Options include `--funding-cancel-seconds 60`, `--funding-interest-bps 1`,
`--funding-clamp-bps 5`, `--cycle-fee-bps 0`, `--funding-cashflows 1`, and
`--max-gross-perp-btc 0`. Per-leg fee overrides remain available for comparisons.
The convergence rule can be enabled explicitly with `--exit-on-equilibrium 1`;
it is disabled in the active strategy. `--premium-events FILE` supplies a custom
causal P-event feed. See the C++ runner's `--help` for all parameters.

## Execution assumptions

Spot uses the latest trade as a midpoint proxy with an assumed **1 bp full
spread**. Every market exit crosses the applicable bid/ask. Zero fees do not
remove crossed spreads. Maker entries allow touch prints, at most 10% of each
observed aggressor print, rounded down to 0.001 BTC; a quote cannot fill at its
creation timestamp. Depth, impact, queue priority, borrow costs, margin and
liquidation are not modeled. Equal BTC removes common dollar price moves;
basis risk remains. Unrealized PnL can be positive or negative.

The graph samples minute equity; maximum drawdown uses every replay event.
Positions, cash and funding carry across day boundaries. Cached Binance archives
remain under `data/binance/maker/`. `backtester/` contains the active strategy and
CSV reader, `tools/` the data/report helpers, and `tests/` the strategy/data checks.
The low-level runner's optional `--details 1` exports full ledgers for debugging.
