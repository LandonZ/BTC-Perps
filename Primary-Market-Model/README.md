# BTC basis strategy

The active midpoint-basis target is **−5 bps versus spot**, with a **2.75 bp entry offset**. Quote the
Binance BTCUSDT perp bid at or below **−7.75 bps**, or its ask at or above
**−2.25 bps**. Quote neither side inside that band. The signal is
`10000 * (perp_mid / spot_mid - 1)`; boundaries are inclusive and are rechecked
at each fill. Every partial fill immediately gets an opposite **equal-BTC spot hedge**.
Each fresh crossing of an entry threshold creates **one 0.01 BTC order budget**.
Partial fills and quote repricing share that budget. Staying beyond the same
threshold does not replenish it; moving back inside the band and crossing again
creates a new event. The signed basis position is limited to **−1 / +1 trade**,
where one trade is **0.01 BTC per leg**. Partial fills may complete that position,
but total open gross perp size cannot exceed 0.01 BTC. An opposite-direction
signal closes the existing trade to flat when the opposite entry band is reached.
That closing signal is consumed; a fresh threshold event can open another trade.
**Maker fees are 0.5 bps; taker fees are 2 bps per fill on both markets.
Crossing equilibrium alone never closes a position.** A completed cycle has one perp maker
entry and three taker fills, totaling a nominal **6.5 bps**. Actual fees use each
fill's executed notional. Open pairs pay only fees on fills already executed;
future exit fees are not charged early.
The 2.75 bp entry offset is a midpoint cushion, not guaranteed profit: executable
spreads also cost money, and funding exits may occur before basis convergence.

## Opposite-signal and funding exits

At the active 2.75 bp deviation, a long basis trade closes when the midpoint
basis reaches −2.25 bp or above; a short closes at −7.75 bp or below. Both legs
close together at executable taker prices. The position stays flat for the rest
of that signal episode, until a fresh threshold event allows entry again.
The continuous runner enables this rule by default; use
`--exit-on-opposite-signal 0` to reproduce the prior funding-only strategy.

During the final **60 seconds before funding**, close both legs only for pairs
expected to **pay any nonzero funding**, including the flat +1 bp rate.
Close a long-perp pair when estimated funding is positive, or a short-perp pair
when it is negative. Zero funding does not trigger an exit. Both legs cross the
spread together and pay their taker fees. Receiving pairs remain open unless
the opposite-signal rule or daily trial end closes them.
Entry quotes pause during that final minute and resume after settlement.

Each active trial starts flat at UTC midnight with fresh quotes, inventory,
entry-event budgets and premium history. At **23:59:59.999 UTC**, any remaining
trade closes at the latest fresh executable bid/ask, charging **2 bp taker fees
on each leg**. The next trial starts from zero. The batch replay is checked
against separate fresh-process one-day replays; it does not carry strategy
state between dates. Day-end liquidation is an additional exit for these trials.

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

The active window is **December 1, 2023 through February 29, 2024 UTC**:
**91 separate daily trials**, each starting and ending flat. The total and
cumulative graph add the daily realized PnLs after all trading fees and funding.
The strategy and threshold were chosen after inspecting this history, so the
results and statistical tests are in-sample diagnostics.

From the repository root:

```bash
python3 -m venv Primary-Market-Model/build/trial-venv
Primary-Market-Model/build/trial-venv/bin/python -m pip install scipy matplotlib
make -C Primary-Market-Model test
make -C Primary-Market-Model daily-trials
```

The profit curve is [results/result.png](results/result.png), also available as
[results/result.html](results/result.html). Daily statistics are exported as
[results/trials.png](results/trials.png) and [results/trials.json](results/trials.json).
`build/current/` retains the summary, sampled equity, daily feed coverage and
input hashes so the charts can be reproduced without another replay.
The active presentation exports are **3840 × 2160 (16:9)**. This run has
its accounting, exposure and statistical results retained in `build/current/`
and `results/trials.json`. The previous uncapped event-based run is preserved
under `results/hypotheses/event-uncapped/`.
The prior funding-only capped run earned **17.97 USDT net marked PnL**: **18.88 USDT funding** less
**0.91 USDT price losses and executed fees**. One short basis position was filled
in three parts and held through the window; there were no long-side entries.
That complete capped replay is retained under `results/position-limit-one/`.

The active **2.75 bp daily-reset experiment** returns **+1013.02 USDT**, fully
realized after **1333.05 USDT execution fees**, including all day-end exits.
Funding paid is **0 USDT** and funding received is **12.39 USDT**. There are
**51 positive trials out of 91**. The long basis side contributes **434.05 USDT**
and the short basis side **578.97 USDT**. Maximum concurrent size remains
**0.01 BTC per leg**, and the cumulative drawdown is **21.47 USDT**. Raw daily
results are in `build/daily-reset-2.75/days/`; the charts, daily CSV and statistical
report are in `results/daily-reset-2.75/`.

The requested one-sided one-sample t-test uses the 91 terminal daily PnLs:
**t = 2.78182**, **df = 90**, **p = 0.00329231**. Mean daily PnL is
**11.13204 USDT**, with a two-sided 95% t interval of
**[3.18195, 19.08213] USDT**. Inventory resets do not prove statistical
independence: adjacent daily correlation remains **0.265**, with a 7-lag
Ljung-Box p-value of **0.03379**. The saved 7-lag Newey-West check gives
**p = 0.02402**; the 7-day centered block bootstrap gives **p = 0.03260**.
These checks do not account for the strategy/threshold selection on this history.

The additional **1.25 bp daily-reset experiment** returns **-164.17 USDT**,
with **28/91 positive days**, **3,029.82 USDT fees**, and zero funding paid.
Its one-sided daily t-test gives **p = 0.666272**. This matches the
2.75 bp daily experiment's dates, input data, engine and other settings; net
difference (1.25 minus 2.75 bp) is **-1,177.19 USDT**.
The additional charts, accounting and comparison are in
[results/daily-reset-1.25/README.md](results/daily-reset-1.25/README.md), including
[the threshold comparison](results/daily-reset-1.25/threshold-comparison.png).
The main presentation files continue to show 2.75 bp.

The additional **daily-reset marked-ending experiments** include each day's
realized plus signed unrealized PnL. They start fresh each day and do not
execute or charge for day-end liquidation. The separate results are:

| Entry deviation | Marked net PnL (USDT) | Positive days | One-sided daily t p |
| --- | ---: | ---: | ---: |
| 1.25 bp | -147.48 | 31/91 | 0.650183 |
| 2.75 bp | +1,029.72 | 62/91 | 0.00289281 |

Marked endings avoid hypothetical closing spreads and fees. Both gains and
losses are included. This is a sum of separate daily valuations, not a
single executable continuous portfolio; market-day independence is not
guaranteed. Intraday trading, funding and input coverage match the prior
liquidation runs for every day. Charts and accounting are in
`results/daily-reset-marked-1.25/` and `results/daily-reset-marked-2.75/`;
[the comparison chart](results/daily-reset-marked-comparison/comparison.png)
shows both terminal policies. Previous main presentation files are retained.
Reproduce with `run_daily_archive_trials.py --close-at-day-end 0` and the
explicit experiment output/report paths documented in each result README.

The **continuous 2.75 bp event experiment** returns **+1,045.27 USDT**
portfolio PnL. Its requested t-test uses **5,159 completed flat-to-flat
trades**, grouping partial fills, with mean **+0.202778 USDT/trade** and
one-sided **p = 5.68586e-273**. The **1 terminal open position(s)**
are excluded from this test and separately reconciled. No daily reset or
forced terminal exit was added. Trade observations may remain dependent;
the saved exit-day cluster/HAC checks address only some dependence.
Reproduce with `make -C Primary-Market-Model event-trials`; charts, event CSV,
accounting and caveats are in
[results/continuous-2.75-events/README.md](results/continuous-2.75-events/README.md).
The main presentation files continue to show the 2.75 bp daily-reset experiment.

The previous **continuous 1.25 bp** opposite-signal strategy returned **−131.49 USDT** across the same
91 days: **2857.38 USDT price PnL before fees**, **18.05 USDT funding**, and
**3006.92 USDT fees**. There are **44 positive days**, with monthly net PnL of
**+170.97 USDT in December**, **+80.22 USDT in January**, and **−382.68 USDT in
February**. Long-perp/short-spot contributes **+180.30 USDT** and
short-perp/long-spot **−311.79 USDT**. Maximum marked drawdown is **856.41 USDT**;
maximum concurrent perp size remains **0.01 BTC**. Complete outputs, daily and
monthly decompositions and the baseline comparison are saved in
`results/continuous-1.25-before-daily-reset/` and `build/opposite-signal-full/`.

That previous run's primary 7-lag Newey-West test gave **p = 0.58075**, with a mean daily PnL
of **−1.44493 USDT** and 95% interval **[−15.34018, +12.45032] USDT**. A centered
7-day block bootstrap gives **p = 0.58807**. The daily observations have positive
serial correlation (adjacent-day correlation **0.379**; 7-lag Ljung-Box
**p = 3.76e−8**). Neither primary test supports a positive mean. The ordinary
t-test is retained in JSON for comparison, not used as the primary display.

For a single-day entry sensitivity sweep, run:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/sweep_entry_deviation.py
```

This randomly selects an archived UTC day before inspecting PnL and tests entry
distances **1–5 bp in 0.25 bp steps** from the −5 bp equilibrium. Every threshold
starts flat on identical ticks with the current fees, funding exits and signed
−1/+1 position limit. Outputs go to `results/entry-deviation-sweep/`: the PNG,
CSV, selection seed, input hashes and each replay's equity and validation.
Use `--day YYYY-MM-DD` or `--seed INTEGER` to reproduce selection, `--stop-bps`
to change the upper bound, and a fresh `--output DIRECTORY` for another sweep.
Net PnL includes executed fees and funding, with open trades marked at the final
prices; it does not assume terminal liquidation. This is a one-day sensitivity
comparison, not an estimate of the best threshold on unseen data.

For five distinct random days plotted together, run:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/sweep_entry_deviation_days.py
```

The combined graph and 85-row CSV go to `results/entry-deviation-five-days/`.
Each day has its own curve and starts flat independently at every threshold.
The random seed, five selected dates, source hashes and per-run accounting
checks are retained. Use `--seed INTEGER`, `--days N`, or a fresh `--output`
directory to reproduce or extend the comparison.

The current five-line illustration replaces December 14 with randomly selected
**February 3, 2024**, retaining the other four dates. It is saved in
`results/entry-deviation-five-days-nearby-opposite-exit/`. The selection changed
after inspecting the earlier plot, so it is an illustration rather than
untouched validation. December remains included in the full backtest.
To reproduce those exact five dates with the current exit rule:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/sweep_entry_deviation_days.py --dates 2024-01-02 2024-02-03 2024-02-12 2024-02-13 2024-02-27 --exit-on-opposite-signal --output Primary-Market-Model/results/reproduced-nearby-sweep
```

An optional **opposite-signal exit** closes the current basis position when the
opposite entry threshold is reached. At 1.25 bp, a long closes at or above
−3.75 bp and a short closes at or below −6.25 bp. Both legs close at executable
taker prices, with 2 bp fees per leg. The closing signal consumes that episode:
the strategy stays flat until the signal leaves that band and a fresh threshold
event occurs. Funding-payer exits remain active, and signed inventory remains
between −1 and +1 trade.

The comparison on the same five selected days can be reproduced with:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/sweep_entry_deviation_days.py --seed 4491175649663169372 --exit-on-opposite-signal --output Primary-Market-Model/results/entry-deviation-five-days-opposite-exit
```

Choose a fresh output directory if reproducing an already-saved run. The archive
and normalized-CSV engines expose `--exit-on-opposite-signal 0|1`; the continuous
archive runner also accepts that option, defaulting to 1 for the active strategy.

The PNG separates **long-perp/short-spot** pairs from **short-perp/long-spot**
pairs. It shows one cumulative net PnL curve per side, including funding and
executed fees. Each side's final PnL and contribution to the total are labeled;
the header separates portfolio price PnL after fees from funding cash.
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

## Hypothesis comparison

The previous **−5 bp target / 1.5 bp offset** run with 0 / 0.5 bp maker/taker fees is preserved under
`results/hypotheses/equilibrium-minus-5/`. That folder includes the original
graphs, equity, summary, validation, input provenance and daily statistics;
`preservation.json` records their SHA-256 hashes. The root reports and
`build/current/` show the latest active settings above.

The **0 bp target** is a separate experiment with entries at **−1.5 / +1.5 bps**.
It uses the same December–February source data, execution assumptions, fees,
unlimited inventory and any-payer pre-funding exits. Run it without replacing
the baseline:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_archive_backtest.py --exit-on-opposite-signal 0 --equilibrium-bps 0 --entry-offset-bps 1.5 --perp-maker-bps 0 --perp-taker-bps 0.5 --spot-taker-bps 0.5 --one-order-per-entry-event 0 --single-basis-position 0 --output Primary-Market-Model/results/hypotheses/equilibrium-0 --report-dir Primary-Market-Model/results/hypotheses/equilibrium-0
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/compare_hypotheses.py Primary-Market-Model/results/hypotheses/equilibrium-minus-5 Primary-Market-Model/results/hypotheses/equilibrium-0 --output Primary-Market-Model/results/hypotheses/comparison.png
```

Each experiment retains its own data and graphs. The comparison validates the
date window, source hashes, observation clock and execution settings, then
separates price PnL after fees from funding. Different inventory paths mean the
dollar PnLs are not returns on equal capital. This is an exploratory comparison
on previously inspected history, not proof that either target is the market's
equilibrium.

### 2 bp entry offset around the −5 bp target

The wider-band experiment uses the same continuous December–February replay,
with long-perp/short-spot entries at or below **−7 bps** and
short-perp/long-spot entries at or above **−3 bps**. Fees, fill assumptions,
funding-only exits and unlimited inventory are unchanged. Its reports are saved
separately under `results/hypotheses/equilibrium-minus-5-offset-2/`.

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_archive_backtest.py --exit-on-opposite-signal 0 --equilibrium-bps -5 --entry-offset-bps 2 --perp-maker-bps 0 --perp-taker-bps 0.5 --spot-taker-bps 0.5 --one-order-per-entry-event 0 --single-basis-position 0 --output Primary-Market-Model/results/hypotheses/equilibrium-minus-5-offset-2 --report-dir Primary-Market-Model/results/hypotheses/equilibrium-minus-5-offset-2
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/compare_hypotheses.py Primary-Market-Model/results/hypotheses/equilibrium-minus-5 Primary-Market-Model/results/hypotheses/equilibrium-minus-5-offset-2 --output Primary-Market-Model/results/hypotheses/equilibrium-minus-5-offset-2/comparison.png
```

The comparison permits different entry targets or offsets while continuing to
require identical source data, observation clocks, coverage and execution rules.
The completed 2 bp replay has **77.13 million USDT net marked PnL**:
**−4.32 million** price PnL after fees plus **81.45 million** funding, versus
79.13 million net for the 1.5 bp offset. Inventory is unlimited, so these dollar
results are not returns on equal capital. See the
[entry-offset comparison](results/hypotheses/equilibrium-minus-5-offset-2/comparison.png)
and the [Victoria branch review](results/hypotheses/equilibrium-minus-5-offset-2/victoria-review/comparison.md)
for the reproduced convergence tests, controlled exit comparison and limitations.

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
remains. Profits are USD amounts under the configured position limit, not returns on a
fixed capital allocation.

The graph samples minute equity and exact observed day-end marks. Maximum
drawdown uses every replay event. Open positions are marked at mids; this omits
future liquidation costs and does not turn unrealized gains into locked profits.

## Daily significance checks

The active observations are **91 separate daily realized PnLs** from fresh
starts, including day-end execution and fees. One observation per day is used
in a single one-sample t-test of mean daily PnL <= 0 versus > 0, with df = 90
and one 95% two-sided t interval displayed. A t-test cannot be estimated from
one day's single terminal PnL alone. The total is the sum of these endpoints.

Resetting inventory removes inventory carryover, but common market conditions
can still correlate adjacent trials. The report retains autocorrelation,
Ljung-Box checks, Bartlett/Newey-West HAC at 3 and 7 daily lags, and centered
circular block bootstrap checks using 3- and 7-day blocks and 20,000 resamples.
The simple t-test is conditional on its independence assumptions; these data
still show serial dependence. Failure to detect correlation would not establish
independence either. The price-only decomposition removes funding cash
from the same trade path; it is not another strategy or another independent
experiment. Resampling does not create additional market history.

Carried inventory, market regimes and changing exposure can make daily PnL
correlated and nonstationary. HAC and bootstrap are diagnostics, not guarantees
of valid inference or realistic execution. The three-month replay should be
judged alongside drawdown, exposure, costs and performance in other unused
periods. Statistical tests live in `tests/trial-analysis-tests.py`.
