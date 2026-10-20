# BTC perp basis strategy

We trade the **basis**, the percentage gap between a BTC perpetual and spot:
`basis_bps = 10000 * (perp_mid / spot_mid - 1)`. Funding pulls the perp back
toward a fair basis (about -5 bp on Binance). When the basis is cheap we rest a
perp bid, when it is rich we rest a perp offer, and every fill is hedged at once
with the same BTC on spot. The pair is closed when the basis returns to fair
value, usually seconds later, so we never hold through a funding payment.

This page is how to run everything on your own machine. The strategy details
and results are in [Primary-Market-Model/README.md](Primary-Market-Model/README.md).

## What's where

| Path | What it is |
|---|---|
| `Primary-Market-Model/backtester/` | C++ replay engine (`basis-maker`): quotes, hedges, exits, fees, funding |
| `Primary-Market-Model/model/bip-funding.*` | Coinbase BIP hourly funding rule |
| `Primary-Market-Model/tools/` | recorders, downloaders, converters and the sweep/report scripts |
| `Primary-Market-Model/tests/` | C++ and Python tests (`make test`) |
| `Primary-Market-Model/results/pitch/` | latest sweep results and charts |
| `data/live/YYYY-MM-DD/` | Coinbase recordings (`events.csv.gz`, `funding.csv.gz`) |
| `data/binance/tardis/YYYY-MM-DD/` | Binance days from Tardis.dev (`events.csv.gz`, `premium-events.csv.gz`) |
| `data/binance/maker/YYYY-MM/` | manifests for Allen's 3-month Binance archive run |

## One-time setup (WSL / Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential clang python3-venv git
cd /mnt/c/workspace/ucf/btc_perps_futures_20261020/BTC-Perps   # your clone
python3 -m venv .venv
source .venv/bin/activate
pip install pandas numpy matplotlib websockets scipy
```

Run `source .venv/bin/activate` again in each new terminal. The Makefile uses
`clang++`; if you only have `g++`, add `CXX=g++` to any `make` command below.

## Check everything works

```bash
make -C Primary-Market-Model test
```

This builds the engine and runs every test. Expect two `PASS:` lines and three `OK`s.

## Rerun the pitch results

```bash
make -C Primary-Market-Model sweep
```

About 2-5 minutes. It runs the strategy on **every** recorded day: each Coinbase
recording in `data/live/` (converted first if new or updated) and each Binance day
in `data/binance/tardis/`, at entry offsets of 0.5-4 bp. Fees are Shreev's (perp
maker 0.5 bp, every taker fill 2 bp, so 6.5 bp per round trip) and latency is zero.
Each day starts and ends flat. It writes `Primary-Market-Model/results/pitch/`:

| File | Contents |
|---|---|
| `summary.md` | basis stats, the pooled results table and net USD per day |
| `sweep.csv` | one row per venue and entry offset, pooled over days |
| `days.csv` | one row per venue, day and offset, with that day's basis stats |
| `markouts.csv` | how far the basis reverts after a deviation, by horizon |
| `basis-binance.png` | the Binance basis on each day against -5 bp |
| `basis-coinbase.png` | the Coinbase basis over the longer recordings |
| `fees.png`, `offsets.png`, `binance-days.png`, `markouts.png` | the pitch charts |

Options go in `SWEEP_ARGS`, for example:

```bash
make -C Primary-Market-Model sweep SWEEP_ARGS="--binance-day 2026-10-01 --binance-day 2026-09-01"
make -C Primary-Market-Model sweep SWEEP_ARGS="--perp-maker-bps 0 --latencies 0,50,250"
```

`--coinbase-day`/`--binance-day` (repeatable) pick days, `--perp-maker-bps`,
`--perp-taker-bps` and `--spot-taker-bps` change fees, and `--latencies` adds
`latency.png` with the hedge and exits repriced that many ms later.

## Get more data

**Coinbase (record it yourself).** Leave this running; Ctrl-C to stop. Keep the
laptop awake, because sleep leaves gaps.

```bash
python3 Primary-Market-Model/tools/record_coinbase.py
```

It writes `data/live/<UTC date>/events.csv` and `funding.csv`. A new file starts
at UTC midnight.

**Binance (download from Tardis.dev).** The first day of every month is free;
other days need a Tardis API key (`--api-key` or `TARDIS_API_KEY`).

```bash
python3 Primary-Market-Model/tools/download_tardis_binance.py --date 2026-11-01
```

Raw downloads (a few GB) go to `data/binance/tardis/<date>/raw/` and are gitignored.

## Run one backtest by hand

Coinbase: convert the recording, then replay it.

```bash
python3 Primary-Market-Model/tools/coinbase_basis_events.py --date 2026-10-19
Primary-Market-Model/build/basis-maker \
    --events data/live/2026-10-19/basis-events.csv.gz \
    --center-halflife-seconds 600 --entry-offset-bps 1.5 \
    --exit-on-equilibrium 1 --close-at-end 1 --exit-before-funding 0 --funding-cancel-seconds 0 \
    --tick 5 --quantity-step 0.01 --participation 1 --fill-at-touch 0 \
    --order-btc 0.01 --max-gross-perp-btc 0.1 --cycle-fee-bps 2 \
    --details 1 --output Primary-Market-Model/build/coinbase-run
```

Binance:

```bash
Primary-Market-Model/build/basis-maker \
    --events data/binance/tardis/2026-10-01/events.csv.gz \
    --premium-events data/binance/tardis/2026-10-01/premium-events.csv.gz \
    --equilibrium-bps -5 --entry-offset-bps 1.5 \
    --exit-on-equilibrium 1 --close-at-end 1 \
    --order-btc 0.01 --max-gross-perp-btc 0.1 --cycle-fee-bps 2 \
    --details 1 --output Primary-Market-Model/build/binance-run
```

Each run writes `summary.json` and `equity.csv`; with `--details 1` it also writes
`pairs.csv` (one row per hedged pair), `fills.csv` and `quotes.csv`. Useful flags
(`build/basis-maker --help` lists them all):

| Flag | Meaning |
|---|---|
| `--entry-offset-bps X` | quote only when the basis is at least X bp from fair value |
| `--equilibrium-bps -5` | fixed fair basis (Binance) |
| `--center-halflife-seconds 600` | fair basis = 10-minute moving average instead (Coinbase) |
| `--exit-on-equilibrium 1` | close a pair when the basis returns to fair value |
| `--close-at-end 1` | close anything still open at the end, so all PnL is realized |
| `--max-hold-seconds N` | close a pair after N seconds regardless |
| `--cycle-fee-bps F` | total fees for a round trip (1 maker + 3 taker fills), split evenly |
| `--perp-maker-bps`, `--perp-taker-bps`, `--spot-taker-bps` | set each fee separately instead |
| `--exit-before-funding 1 --funding-exit-min-bps X` | exit payers before funding only if the rate is at least X bp |
| `--max-gross-perp-btc B` | cap on open perp BTC (0 = unlimited) |

## Allen's 3-month Binance run

The original continuous Dec 2023 - Feb 2024 replay (carry-style, holds through
funding) is documented in [Primary-Market-Model/README.md](Primary-Market-Model/README.md).
It downloads several GB of archives:

```bash
python3 -m venv Primary-Market-Model/build/trial-venv
Primary-Market-Model/build/trial-venv/bin/python -m pip install scipy matplotlib
make -C Primary-Market-Model run
```

## Committing new data

Data CSVs are gitignored because they are large. Compress them and add them
explicitly (GitHub rejects files over 100 MB):

```bash
gzip -k data/live/2026-10-20/*.csv
git add -f data/live/2026-10-20/*.csv.gz
git commit -m "Add Coinbase recording for 2026-10-20"
```

Every tool reads the `.gz` files directly. Converted `basis-events.csv.gz` files
are regenerated by the sweep and stay ignored.
