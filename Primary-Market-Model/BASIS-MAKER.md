# Paired basis entries at -5 bps equilibrium

The active strategy enters a passive BTCUSDT perpetual position and immediately
hedges each partial fill by crossing Binance BTCUSDT spot. The user specified
**equal BTC quantities**, **no exits**, and **zero funding cash flows** for this
scenario. Orders are simulated; nothing is submitted to an exchange.

## Entry rule

```text
basis_bps = 10000 * (perp_mid / spot_mid - 1)
equilibrium_perp_price = spot_mid * (1 - 5/10000)

basis < -5: join best perp BID; on fill, SELL the same BTC at spot BID
basis > -5: join best perp ASK; on fill, BUY  the same BTC at spot ASK
basis = -5: cancel entry quotes
```

Equilibrium is always -5 bps for both directions. There is no earlier +/-5 bp
trading gate and no spot-plus/minus-fee quote. The order price follows the actual
top of the perp book. Quotes are post-only. Actual observed trade prints, not
quote creation, cause simulated fills. The model uses zero decision/hedge latency.

Each partial perp fill causes exactly one equal-BTC spot hedge at the same
timestamp. Spot shorts assume borrowed BTC is available. Borrow limits, borrow
interest, market impact, hedge failures, margin and liquidation are not modeled.

## Sizing and exposure

For a perp fill of `q` BTC at `P` and an opposite spot fill at `S`:

```text
perp quantity = q BTC
spot quantity = q BTC (opposite sign)
perp notional = q * P
spot notional = q * S
notional difference = q * (P-S)
```

The quantities match, so an equal absolute price move in both markets cancels:
`price_PnL = direction * q * [(P_now-P_entry) - (S_now-S_entry)]`.
The dollar notionals differ because the prices differ. This implements the
user's clarification to match BTC quantities rather than dollars. Basis changes,
funding if enabled in a future scenario, execution costs and borrow risk remain.

## Fees and scenario assumptions

Defaults charge the **actual filled notional** of each entry leg:

- Perp maker: **2 bps** per fill, using the regular-user example in
  [Binance's futures fee explanation](https://www.binance.com/en/support/faq/detail/360033544231).
- Spot taker: **10 bps** per fill, using the no-BNB regular tier in
  [Binance's spot fee schedule](https://www.binance.com/en/fee/trading).

These are scenario rates, not verified account-specific or historical discounts.
There are no exit fees because the scenario has no programmed exits. Fees are
accounted for, not used to silently add a minimum convergence edge or change
the requested entry rule. Entry fees total roughly 12 bps of one leg's notional.

The [user-supplied funding reference](https://www.binance.com/en/support/faq/detail/360033525031)
defines the schedule semantics. Entry quotes are canceled **60 seconds before**
the next known funding time, with a configurable buffer. The archive's latest
observed interval updates the schedule; the historical default is eight hours.
If an expected funding time passes without a new schedule event, entry stays
blocked. Actual historical funding rates never affect PnL in this explicit
zero-funding scenario. Existing pairs remain open through funding times.

There are no exits on convergence, profit, timeout, funding or replay end.
An opposite entry can economically offset an earlier net position; the ledger
still records both as separate entry fills, not a programmed exit.

## Backtest data and fill assumptions

The retained Binance samples contain perp bid/ask updates, perp trades, spot
trades and funding timestamps. Spot bid/ask history was unavailable from checked
public endpoints. Therefore price-only `S` events use the latest spot trade as
a **midpoint proxy**, with an explicitly assumed **1 bp full spot spread**:

```text
proxy spot bid = latest spot trade * (1 - 0.5/10000)
proxy spot ask = latest spot trade * (1 + 0.5/10000)
```

True spot books can be supplied using `S` events with bid and ask. These take
precedence over the proxy spread. Results identify proxy and real quote counts.
The spot crossing results are illustrative, not proof of actual executable spot fills.

Default maker execution allows a trade at our price, because we join the best
bid/ask. A compatible aggressor print can fill at most 10% of its observed BTC,
remaining quote size and remaining gross capacity, rounded down to 0.001 BTC.
This approximates queue allocation without reconstructing queue priority.
`--fill-at-touch 0` requires a strict print through the quote as a sensitivity.
Decimal tick equality is tested with numerical tolerance.

Order size is 0.01 BTC. The **gross cumulative perp entry cap is 0.05 BTC**;
both long and short entries consume it. It is not recycled through opposite
entries, since this scenario has no explicit exit rule. This bounds accumulated
entries and fees. Partial fills of a valid order are allowed. Hedge orders that
cannot satisfy the spot minimum notional are not simulated.

## Run and output

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py
make -C Primary-Market-Model test
make -C Primary-Market-Model backtest
```

The default window is 2023-07-20 00:00-01:00 UTC and the output directory is
`Primary-Market-Model/results/binance-basis-maker/`. For the retained second window:

```bash
make -C Primary-Market-Model basis-maker \
  BASIS_MAKER_ARGS="--events ../data/binance/maker/2023-07-20/events-spot-trades-07-09.csv --output results/binance-basis-maker-07-09"
```

Use `./Primary-Market-Model/build/basis-maker --help` for fee, size, spread,
participation and cancellation parameters.

To prepare the entire UTC day and replay it with zero entry fees:

```bash
python3 Primary-Market-Model/tools/download_binance_maker.py \
  --hours 24 --output data/binance/maker/2023-07-20/events-spot-trades-full-day.csv
make -C Primary-Market-Model basis-maker \
  BASIS_MAKER_ARGS="--events ../data/binance/maker/2023-07-20/events-spot-trades-full-day.csv --perp-maker-bps 0 --spot-taker-bps 0 --output results/binance-basis-maker-zero-fees-full-day"
```

Preparation uses a stable external merge sort with bounded RAM and file handles.
The full-day replay preserves the 0.05 BTC gross entry cap and no-exit rule; once
the cap is reached, it continues marking the held positions without adding more
entries. It is therefore a full-day holding-period evaluation of that capped
strategy, rather than continuous entry throughout the day.

Outputs are `fills.csv` (two immediate legs per pair), `pairs.csv` (BTC quantities,
both notionals and their difference), `quotes.csv`, `equity.csv` and `summary.json`.
Net PnL marks all remaining net positions at the current midpoints and subtracts
paid entry fees. It is not a completed-trade return and includes no future exit
fees. `delta_btc` must remain zero throughout the replay.

The earlier unhedged spot-fee maker remains available via `make maker-backtest`;
see [its documentation](BINANCE-MAKER.md). The historical Coinbase simulator is
still `make basis-backtest` and is separate from this tick-based paired strategy.
