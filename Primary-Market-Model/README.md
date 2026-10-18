# BTC basis strategy

Equilibrium is **−5 bps versus spot**, with a **1.5 bp entry offset**. Quote the
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
Pass `--png` to `tools/run_backtest.py` to regenerate its sibling PNG as well;
use the analysis environment below, which includes matplotlib.

Options include `--entry-offset-bps 1.5`, `--funding-cancel-seconds 60`, `--funding-interest-bps 1`,
`--funding-clamp-bps 5`, `--perp-maker-bps 0`, `--perp-taker-bps 0.5`,
`--spot-taker-bps 0.5`, `--funding-cashflows 1`, and
`--max-gross-perp-btc 0`. Per-leg fee overrides remain available for comparisons.
The convergence rule can be enabled explicitly with `--exit-on-equilibrium 1`;
it is disabled in the active strategy. `--premium-events FILE` supplies a custom
causal P-event feed. See the C++ runner's `--help` for all parameters.

## Execution assumptions

Spot uses the latest trade as a midpoint proxy with an assumed **1 bp full
spread**. Every market exit crosses the applicable bid/ask. Execution fees are
additional to crossed spreads. Maker entries allow touch prints, at most 10% of each
observed aggressor print, rounded down to 0.001 BTC; a quote cannot fill at its
creation timestamp. Depth, impact, queue priority, borrow costs, margin and
liquidation are not modeled. Equal BTC removes common dollar price moves;
basis risk remains. Unrealized PnL can be positive or negative.

The graph samples minute equity; maximum drawdown uses every replay event.
Positions, cash and funding carry across day boundaries. Cached Binance archives
remain under `data/binance/maker/`. `backtester/` contains the active strategy and
CSV reader, `tools/` the data/report helpers, and `tests/` the strategy/data checks.
The low-level runner's optional `--details 1` exports full ledgers for debugging.

## Repeated trials and significance

`tools/run_trials.py` replays each UTC day separately from flat, using the same
funding-only strategy, 0 bps maker / 0.5 bps taker fees, and uncapped 0.01 BTC entry quotes. Open inventory
is marked at the final mid; it is not liquidated. This estimates **24-hour marked
PnL from a flat start**, which differs from continuously holding positions for a
month. Repeating the same deterministic replay does not create new observations.

From the repository root, create an isolated analysis environment and run:

```bash
python3 -m venv Primary-Market-Model/build/trial-venv
Primary-Market-Model/build/trial-venv/bin/python -m pip install scipy matplotlib
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tests/trial-analysis-tests.py
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_trials.py --workers 4
```

The default is July 1–31, 2023; `--start` and `--end` choose other complete cached
days. Daily full-day manifests, source archives and monthly premium events must
already be prepared. The runner verifies archive hashes, regenerates and deletes
temporary tick CSVs, and safely reuses saved daily summaries in `build/` only when
binary, premium data, source hashes and settings agree. It writes
[results/trials.png](results/trials.png) and [results/trials.json](results/trials.json).
Use `--from-results results/trials.json` to redraw an existing analysis.
Per-leg overrides use `--perp-maker-bps`, `--perp-taker-bps` and
`--spot-taker-bps`; they default to 0, 0.5 and 0.5 bps respectively. Fees are
part of the daily-summary cache signature.
`--entry-offset-bps` defaults to 1.5 around the fixed −5 bp equilibrium and is
also included in the cache signature. Every replay summary reports the entry
thresholds and checks every partial-fill lot for entry-band violations.
`--continuous-output Primary-Market-Model/build/month`
also replays the same input dates without daily resets, using identical fees.
That option temporarily retains the normalized ticks for the continuous replay
and deletes them when the run finishes; choose dates within one calendar month.

The primary test is a one-sided one-sample t-test: mean daily PnL <= 0 versus > 0
at 5%. The report includes a two-sided 95% t interval, Newey-West/Bartlett HAC
standard errors at 3 and 7 daily lags (asymptotic normal reference), and centered
circular block bootstrap diagnostics with 3- and 7-day blocks, 20,000 resamples
and a fixed seed. Bootstrap repetitions reuse observed history; they are not
additional market trials. These checks cannot establish independence,
stationarity or validity of the execution model.

July was already used to develop this strategy, so these are **in-sample
diagnostics**. USD profits are not returns on fixed capital. Assumed fee rates,
unlimited capital/borrow, mid marks and omitted impact/borrow costs limit any
claim about live profitability. Unseen months and realistic costs are needed
before treating a significant test as evidence of a tradable edge.
