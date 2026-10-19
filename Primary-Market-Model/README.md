# BTC basis strategy

The configured midpoint-basis target is **−5 bps versus spot**, with a **1.5 bp entry offset**. Quote the
Binance BTCUSDT perp bid at or below **−6.5 bps**, or its ask at or above
**−3.5 bps**. Quote neither side inside that band. The signal is
`10000 * (perp_mid / spot_mid - 1)`; boundaries are inclusive and are rechecked
at each fill. Every partial fill immediately gets an opposite **equal-BTC spot hedge**.
Quotes are 0.01 BTC; concurrent pairs and position size are uncapped by default.
**Maker fees are 0 bps; taker fees are 0.5 bps per fill on both markets.
Convergence never closes a position.** A completed cycle has one free perp maker
entry and three taker fills, totaling a nominal **1.5 bps**. Actual fees use each
fill's executed notional. Open pairs pay only fees on fills already executed;
future exit fees are not charged early.
The 1.5 bp entry offset is a midpoint cushion, not guaranteed profit: executable
spreads also cost money, and funding exits may occur before basis convergence.

## Funding-only exits

During the final **60 seconds before funding**, close both legs only for pairs
expected to **pay any nonzero funding**, including the flat +1 bp rate.
Close a long-perp pair when estimated funding is positive, or a short-perp pair
when it is negative. Zero funding does not trigger an exit. Both legs cross the
spread together and pay their taker fees. All receiving pairs remain open,
including at the end of the replay.
Entry quotes pause during that final minute and resume after settlement.

Binance's eight-hour funding calculation is
`F = P + clamp(I - P, -5 bps, +5 bps)`, with the standard interest component
`I = 1 bp`. Therefore averaged premium **P from −4 to +6 bps** produces a flat
**1 bp** funding rate. Beyond those boundaries the rate scales linearly. Positive
estimated funding selects long-perp payers for exit; negative funding selects
short-perp payers. Receiving pairs stay open. The flat rate is low, but not zero.
See [Binance's funding explanation](https://www.binance.com/en/support/faq/detail/360033525031).

Under that standard formula, zero funding occurs at **averaged premium P =
−5 bps**. This does not establish that a current perp/spot midpoint basis of
−5 bps is funding-neutral or a price equilibrium. The premium uses order-book
impact prices versus a multi-exchange spot index, averaged over the funding
window. Our entry midpoint basis uses different prices and no such average.
The illustrative [funding curve](results/funding-curve.png) shows the formula;
it is not an empirical fit between our entry signal and realized funding.

The exit signal uses a causal, linearly weighted prefix average of completed
**official premium-index 1-minute CLOSEs** within the funding period. A candle
becomes available at `close_time + 1 ms`. This approximates Binance's more
frequent premium sampling; it is not an exact historical funding forecast.
It is separate from the instantaneous perp/spot basis used for entries. Missing,
incomplete or stale premium history cannot trigger a funding exit. The upcoming
final settlement rate is never used early. Forecast errors or stale execution
references can still leave residual payments; the replay audits actual paid
and received funding separately instead of assuming the exit rule makes all
payments zero.

Held positions pay or receive the **recorded funding at settlement**. Funding
cash is realized immediately; open-position unrealized PnL contains price marks
and paid entry fees. Closed-pair lifetime PnL also attributes that pair's funding
without counting it twice in the portfolio. The funding mark is the settlement
minute's mark-price OPEN, a proxy for the official funding mark. This window uses
00:00, 08:00 and 16:00 UTC; the engine follows archived interval updates.


## Run

The active window is **December 1, 2023 through February 29, 2024 UTC**: 91 days
from one flat start. Positions, cash and funding carry across every day and
month; there is no terminal liquidation. The strategy settings above are fixed
throughout each replay. The exit rule has been revised on this already-inspected
history, so these are in-sample comparisons rather than another untouched period.

From the repository root:

```bash
python3 -m venv Primary-Market-Model/build/trial-venv
Primary-Market-Model/build/trial-venv/bin/python -m pip install scipy matplotlib
make -C Primary-Market-Model test
make -C Primary-Market-Model run
```

The profit curve is [results/result.png](results/result.png), also available as
[results/result.html](results/result.html). Daily statistics are exported as
[results/trials.png](results/trials.png) and [results/trials.json](results/trials.json).
`build/current/` retains the summary, sampled equity, daily feed coverage and
input hashes so the charts can be reproduced without another replay.

The PNG separates **long-perp/short-spot** pairs from **short-perp/long-spot**
pairs. It shows cumulative price PnL (realized plus unrealized) and unrealized
price PnL on still-open pairs, with funding cash in a separate final table.
The two direction panels use separate, explicitly labeled vertical scales.
Both legs and executed fees are included in each direction. An exited pair's
price result moves from unrealized to realized; it does not disappear when its
direction becomes flat. These are attributions of the same two-sided replay,
not separate experiments that disable one entry direction. The HTML also lets
you switch between unrealized and total price curves. `basis_sides` in
`summary.json` and the direction columns in `equity.csv` retain this breakdown.

To redraw saved results:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_backtest.py --from-results Primary-Market-Model/build/current --png
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_trials.py --from-results Primary-Market-Model/results/trials.json
```

Add `--funding-curve` to the saved-result command to redraw the standard
eight-hour funding illustration alongside the report.

## Archive inputs

Each month has six checksum-verified Binance source ZIPs: perp best-bid/ask
updates, perp aggregate trades, spot aggregate trades, mark-price minutes,
funding records and premium-index minutes. They remain under
`data/binance/maker/YYYY-MM/archives/`. Each `month-inputs.manifest.json` records
source URLs, SHA-256 hashes, byte sizes and minute/funding coverage checks.

Download or verify a month without retaining expanded tick CSVs:

```bash
python3 Primary-Market-Model/tools/prepare_binance_basis_month.py --month 2023-12 --archives-only
python3 Primary-Market-Model/tools/prepare_binance_basis_month.py --month 2024-01 --archives-only
python3 Primary-Market-Model/tools/prepare_binance_basis_month.py --month 2024-02 --archives-only
```

`tools/run_archive_backtest.py` verifies the hashes, streams ZIP contents through
`unzip`, stably sorts every source using bounded-memory chunks and disk-backed
merge passes, then merges funding, premium, spot, quotes and trades into the
existing C++ strategy engine. Monthly quote files can be substantially out of
order; the complete sort preserves equal-timestamp input order without assuming
a maximum delay. No ticks are sampled or fabricated. Transaction timestamps
remain the clock for zero-latency replay. Every requested UTC day
must contain quotes, spot trades and perp trades before results are published.
`coverage.csv` reports counts, observed start/end times and maximum within-day
gaps; a complete archive checksum alone does not prove gap-free market data.
The retained files have quote gaps on December 7, January 3 and February 28,
with the longest lasting 43.1 minutes. Stale books pause new entries, while held
inventory remains marked at last observed prices; interim equity and drawdown
can therefore be distorted during gaps. The report retains these observations
and discloses the gap instead of inventing replacement quotes.

The full quote CSVs expand to hundreds of gigabytes. Compact temporary 32-byte
sort records avoid retaining the expanded CSVs and are deleted after replay.
Two sorting jobs run at a time, with at most 64 files per merge pass.
Fill totals and quote state are retained without
optional per-fill/quote logs; pairs and all accounting remain in the engine.
Tests compare the streaming path against the original normalized CSV path.
The CSV runner remains available for explicit custom inputs and fee comparisons.

Binance's public quote archives cover these historical months. They do not
provide the newer quote observations needed for the same maker-fill test; see
[the public-data discussion](https://github.com/binance/binance-public-data/issues/334).
These additional dates were not used to tune the entry band, but they remain
historical simulations, not current-market evidence.

## Execution assumptions

Spot uses the latest observed trade as a midpoint proxy with an assumed **1 bp
full spread**. Every taker fill crosses the applicable bid/ask and pays the
configured fee. Maker entries allow touch prints, at most 10% of observed
aggressor quantity, rounded down to 0.001 BTC. A quote cannot fill at its creation
timestamp. Queue priority, depth, impact, borrowing costs, capital, margin and
liquidation are not modeled. Equal BTC removes common price moves; basis risk
remains. Profits are USD amounts under unlimited inventory, not returns on a
fixed capital allocation.

The graph samples minute equity and exact observed day-end marks. Maximum
drawdown uses every replay event. Open positions are marked at mids; this omits
future liquidation costs and does not turn unrealized gains into locked profits.

## Daily significance checks

The active daily observations are **changes in marked portfolio equity** from
the continuous run. They sum to its total profit. Each day's change includes
that day's funding and executed fees while retaining existing inventory. This
replaces the earlier July experiment that restarted flat every day; those are
different horizons and should not be combined as identical trials.

The report includes a one-sided t-test of mean daily PnL <= 0 versus > 0, a
95% two-sided t interval, Bartlett/Newey-West HAC checks at 3 and 7 daily lags,
and centered circular block bootstrap checks with 3- and 7-day blocks, 20,000
resamples and a fixed seed. The price-only decomposition removes funding cash
from the same trade path; it is not another strategy or another independent
experiment. Resampling does not create additional market history.

Carried inventory, market regimes and changing exposure can make daily PnL
correlated and nonstationary. HAC and bootstrap are diagnostics, not guarantees
of valid inference or realistic execution. The three-month replay should be
judged alongside drawdown, exposure, costs and performance in other unused
periods. Statistical tests live in `tests/trial-analysis-tests.py`.

## Coinbase: BIP perp vs BTC-USD spot

INTX BTC-PERP has been paused since 2026-10-01, so the Coinbase leg uses the
Coinbase Derivatives nano BTC perp-style future `BIP-20DEC30-CDE` (0.01 BTC per
contract, $5 tick, hourly funding) against `BTC-USD` spot.
`model/bip-funding.{h,cpp}` implements Coinbase's published funding rule:

```text
futures mark = 3m VWAP | else 3m TWAP of mid | else spot mark + previous gap
spot mark    = 3m VWAP | else 3m TWAP of mid | else futures mark + previous gap
Premium      = mean over the hour's 20 samples of (futures mark - spot mark) / spot mark / 24
Rate         = 0.75 * Premium + 0.25 * Previous rate     (no published cap)
Long pays    = contracts * 0.01 * mark * Rate            (negative rate: long receives)
```

Unlike Binance there is no interest term or clamp, so the formula's
funding-neutral basis is **0 bps**, not -5. Hourly funding is about the basis
divided by 24, a fraction of a basis point. `FundingEstimator` builds the marks and
the projected rate from streamed trades and quotes; it publishes no rate for an
hour with no futures data or for a partial first hour.

Funding accrues hourly and is cash-settled at the midday and end-of-day margin
runs. Open questions: whether "spot" means Coinbase BTC-USD or the product API's
`index_price`, and whether a position flat at the hour boundary avoids that
hour's accrual.

### Recording Coinbase ticks

Run on a laptop (needs `pip install websockets`; no API key):

```bash
python3 Primary-Market-Model/tools/record_coinbase.py      # Ctrl-C to stop
```

It writes `data/live/YYYY-MM-DD/events.csv`
(`recv_ms,exch_ms,kind,product,bid,bid_qty,ask,ask_qty,price,size,side`: top-of-book
changes `Q` and trades `T` for both products, from the public level2 and
market_trades channels) and `funding.csv`, a 15-second REST poll of the BIP
product's `funding_rate`, `funding_time` and `index_price`. Both are gitignored.
`recv_ms` is the laptop clock; a sleeping laptop leaves gaps.
