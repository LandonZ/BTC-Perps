# Kalshi Market Model

Zero-cost basis backtest of the Kalshi BTC perp (`KXBTCPERP`) against Coinbase
`BTC-USD` spot. It applies the same rule as `Primary-Market-Model/results/basis-both-5m`:
short the perp / buy spot when the basis is above +threshold, buy the perp / short
spot when it is below -threshold, and exit on first profit, convergence, or the
holding deadline.

## You have to rerun the backtests to see the full results

To keep the repo small, git only stores each run's `report.md`, `summary.json`
and `pnl.png`. The large outputs are gitignored, and the
"Audit files" links at the bottom of each `report.md` stay broken until you
regenerate them:

- `trades.csv`: every trade
- `equity.csv`: minute-by-minute mark-to-market PnL
- `pnl.svg`: vector version of the chart

### 1. Download the market data

The minute candles are gitignored too (only the funding files are committed).
From the repo root:

```sh
python3 data/fetch_data.py all
```

The backtester needs `data/coinbase/BTC-USD_spot_1m.csv`,
`data/kalshi/KXBTCPERP_candles_1m.csv` and `data/kalshi/KXBTCPERP_funding.csv`.

### 2. Run the backtests

From this folder (`Kalshi-Market-Model`):

| Command | What it reruns |
|---|---|
| `make everything` | all of the below |
| `make report` | the four headline runs (table below), with charts and reports |
| `make sweeps` | both parameter sweeps |
| `make sweep-exit` | `results/exit1-sweep`: entry threshold (2/3/5/8 bps) x profit exit on/off x mid/touch fills |
| `make sweep-hold` | `results/exit1-hold`: touch fills, entry 3/5 bps, max hold 5/15/30/60/240 min |
| `make help` | lists these commands |

Each one builds `build/kalshi-backtester` first if needed. Python 3 with
`matplotlib` is required for the charts.

The headline runs from `make report`:

| Folder | Fills | Start |
|---|---|---|
| `results/basis-both-5m` | mid (idealized) | Kalshi launch |
| `results/basis-both-5m-touch` | touch (pay the spread) | Kalshi launch |
| `results/basis-both-5m-from-jun09` | mid | 2026-06-09 |
| `results/basis-both-5m-touch-from-jun09` | touch | 2026-06-09 |

For a one-off run with your own settings, pass options through `make backtest`,
e.g. `make backtest BACKTEST_ARGS="--threshold-bps 3 --fill touch --output results/my-run"`.
Run `./build/kalshi-backtester --help` for every option.

### 4. Maker simulation (optional)

`backtester/kalshi-maker.cpp` rests post-only perp quotes around a fair value
from Coinbase spot and hedges each fill on spot, instead of crossing the spread.
It needs tick data, which only goes back about a week. From the repo root:

```sh
python3 data/fetch_data.py kalshi --trades-start 2026-10-05
python3 data/fetch_data.py coinbase-ticks --trades-start 2026-10-05 --end 2026-10-13
```

Then `make maker-report` here reruns every scenario into `results/maker/` and
writes `results/maker/report.md`, which lists the fill, latency and fee assumptions.

## Layout

- `backtester/kalshi-backtester.cpp`: the backtest (data loading, rule, outputs)
- `backtester/kalshi-maker.cpp`: the maker simulation on tick data
- `tools/maker_report.py`: compares every run under `results/maker`
- `model/`: a rolling-mean basis model (not used by the backtester yet)
- `tools/plot_backtest.py`: writes `pnl.png`, `pnl.svg` and `report.md` for a results folder
- `results/`: one folder per run
