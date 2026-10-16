# Binance spot-anchored perp maker

The current model quotes **Binance USD-M BTCUSDT perpetuals**, using spot as the
reference. It places no spot orders and simulates all perp orders.

## Current rule

Let `S` be spot midpoint, `M` perp midpoint and `f` the maker fee per fill:

```text
basis_bps = 10000 * (M/S - 1)
bid = floor_to_tick(S * (1-f))
ask = ceil_to_tick(S * (1+f))
quote only if -5 <= basis_bps <= +5
```

Both candidate endpoints must also remain inside spot +/-5 bps. If either
endpoint falls outside, skip the pair; do not clip prices to erase the rule.
Default fees are **2 bps per executed fill** (0.02%, no BNB discount), using the
regular-user example in [Binance's fee explanation](https://www.binance.com/en/support/faq/detail/360033544231).
This is a scenario fee, not an account-verified tier or historical fee schedule.

At spot 100,000, quote 99,980 bid and 100,020 ask: about **4 bps full spread**.
This covers about 4 bps of static buy/sell fees and has effectively no additional
profit margin before tick rounding. Actual realized profits depend on fills and
subsequent prices.

The current rule has **zero decision latency and zero reprice delay**. It adds
no risk buffer, target-profit margin, funding reserve, inventory skew or
last-settled-rate filter. The older cost-budget rule remains explicitly available
with `--quote-rule cost-budget`; see [legacy model](BINANCE-COST-BUDGET.md).

If one candidate would immediately cross the perp book, reject that side as
post-only. Keep the other side at its spot-fee price. Do not move either price
to the book or turn it into a taker order. Inventory size caps still apply.

## Funding

The user-supplied [Binance funding reference](https://www.binance.com/en/support/faq/detail/360033525031)
defines payment direction and timing. The +/-5 bp midpoint rule is the user's
proxy for the flat-funding regime. It cannot guarantee zero actual funding:
Binance uses an averaged impact premium, and its standard eight-hour plateau
has a nonzero interest component. Historical rates are charged at their recorded
settlements on actual inventory:

```text
funding_cash = -signed_BTC * recorded_mark * recorded_rate
```

There is no prorated hourly charge. Intervals are read from archive rows; their
future duration is assumed to persist until another row is observed. The mark
is the settlement-minute mark-price candle open, a proxy for the official
charged mark. The FAQ's possible 15-second charge-time deviation is not simulated.

## Data and reproducibility

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py
make -C Primary-Market-Model test
make -C Primary-Market-Model maker-backtest
```

Defaults prepare and run **2023-07-20 00:00-01:00 UTC** with zero latency,
2 bps maker fees, 0.01 BTC order size, a 0.05 BTC absolute inventory cap and
10% participation in eligible trades. Results go to
`Primary-Market-Model/results/binance-maker-spot-fee/`.

The public archive provides perp bookTicker, perp aggTrades, spot aggTrades,
mark-price candles and funding. Archives are SHA-256 verified and retained.
The source manifest records URLs, hashes, event counts and clock assumptions.

**Spot data limitation:** public historical spot bid/ask archives were unavailable
from the checked Binance URLs. Free third-party sample endpoints were also
inaccessible. The downloadable replay therefore uses the **latest observed spot
trade as a proxy**, not a real midpoint. This is faster than the previous
completed-minute-close proxy, but the result is not an exact midpoint backtest.
True spot quote data can be supplied as `S` events with `bid` and `ask`; the
engine computes their midpoint and distinguishes true and proxy inputs in the
summary. Price-only `S` events are always labeled proxies.

To prepare the two-hour comparison without changing the default window:

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py \
  --start-hour 7 --hours 2 \
  --output data/binance/maker/2023-07-20/events-spot-trades-07-09.csv
make -C Primary-Market-Model maker-backtest \
  MAKER_ARGS="--events ../data/binance/maker/2023-07-20/events-spot-trades-07-09.csv --output results/binance-maker-spot-fee-07-09"
```

`--spot-reference minute-close` preserves the older candle-based comparison.
It writes `events.csv`; the default last-trade reference writes
`events-spot-trades.csv`. These files must not be mixed when comparing rules.

## Fill simulation and output

- Quotes and cancels arrive immediately at zero latency. An updated reference
  already known within an event batch replaces old quotes before later trades.
  A new quote cannot fill from a print at its creation timestamp.
- Default execution requires a print strictly through the quote, with a compatible
  aggressor direction. `--fill-at-touch 1` is an optional more optimistic variant.
- Partial size is at most observed trade BTC times participation, remaining quote
  size and inventory capacity, rounded down to 0.001 BTC. Queue priority is not
  reconstructed. Assumed minimum new-order notional is 100 USDT; tick is 0.1.
- Funding is paid or received when recorded settlement events arrive. Net PnL is
  trade cash + inventory marked at perp mid + funding - actual filled fees.
  Inventory remains open at the end, without a fictitious liquidation fill.

Outputs are `quotes.csv`, `fills.csv`, `equity.csv`, and `summary.json`. The quote
log shows candidate prices, per-side passive eligibility and skip reasons. It
logs changes rather than every identical decision. Summaries identify the quote
rule, actual latency and whether spot used real bid/ask midpoints or a proxy.

Run `./Primary-Market-Model/build/maker-backtester --help` for all settings.
Source files: `model/maker-model.{h,cpp}`, `backtester/maker-backtest.{h,cpp}`,
`backtester/maker-backtest-cli.cpp`, and `tools/download_binance_maker.py`.
