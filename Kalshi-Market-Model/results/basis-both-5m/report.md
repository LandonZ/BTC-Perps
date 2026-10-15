# Kalshi BTC basis backtest (mid fills)

**Total PnL: +185.887246 USD**, consisting of
+185.590500 basis-price PnL and +0.296746 funding cash flows.

![Mark-to-market PnL](pnl.png)

| Metric | Result |
|---|---:|
| UTC data start | 2026-06-03T14:02:00Z |
| UTC data end | 2026-10-12T20:07:00Z |
| Perp fill price | bid/ask midpoint (idealized, like the primary run) |
| Quantity per leg | 0.01 BTC |
| Maximum holding period | 5 minutes |
| Average measured holding period | 1.999 minutes |
| Longest measured trade | 119 minutes |
| Mean Kalshi spread at entry | 3.489 bps |
| Trading/carry costs | 0 bps |
| Funding events crossed | 3 |
| Trades | 1111 |
| Win rate | 96.85% |
| Mean trade PnL | +2.511449 bps |
| Maximum observed minute mark-to-market drawdown | 81.150400 USD |
| Matched minute observations | 186,667 |
| Missing matched minutes inside covered span | 2,339 |
| Kalshi minutes dropped for empty/crossed quotes | 52 |
| Final pairs closed early at data end | 0 |
| Late exits (deadline quote missing) | 9 |

The measured trades crossed 3 8-hour funding events; actual cash flows are included.

| Direction | Trades | Basis-price PnL | Funding PnL | Total PnL |
|---|---:|---:|---:|---:|
| Buy basis | 34 | +48.322500 | +0.000000 | +48.322500 |
| Sell basis | 1077 | +137.268000 | +0.296746 | +137.564746 |

## Rule tested

The same rule as `Primary-Market-Model/results/basis-both-5m`, with the Kalshi
perp in place of Coinbase `BTC-PERP-INTX`. Check each available minute open, with
`basis = 10,000 * (perp / spot - 1)`:

- Buy basis: **long perp / short spot** when basis is below **-5 bps**.
- Sell basis: **short perp / long spot** when basis is above **+5 bps**.

Each entry also requires positive theo edge above 0 bps:
the basis move to the **-5 / +5 bps** target, minus funding the
side would pay if an 8-hour Kalshi funding boundary falls inside the
5-minute window (the last recorded rate is persisted).

Close at the first available minute with positive total PnL (including funding)
when profit exits are enabled, as in this run,
when long basis reaches -5 bps or higher or
short basis reaches +5 bps or lower,
or at the 5-minute deadline.
Never close and reopen in the same minute. Hold one equal-BTC pair at a time.
No future prices select an entry. A final open pair is closed at data end.

If no matched price exists at the exact deadline, the pair closes at the next
observed minute (LATE_TIMEOUT). Trades are never dropped based on whether later
data exists, so the sample is not conditioned on future information.

| Exit reason | Trades |
|---|---:|
| CONVERGED | 1 |
| LATE_TIMEOUT | 9 |
| PROFIT | 1070 |
| TIMEOUT | 31 |

### Worst trades

| Entry (UTC) | Side | Basis entry → exit (bps) | Entry spread (bps) | Net PnL | Exit |
|---|---|---|---:|---:|---|
| 2026-06-05T02:50:00Z | Sell | +25.5 → +327.3 | 9.3 | -18.8765 | TIMEOUT |
| 2026-09-10T07:01:00Z | Sell | +5.3 → +46.7 | 7.4 | -3.2288 | LATE_TIMEOUT |
| 2026-06-05T02:40:00Z | Sell | +10.7 → +26.3 | 0.2 | -0.9792 | TIMEOUT |
| 2026-06-04T19:29:00Z | Sell | +5.5 → +15.5 | 7.2 | -0.6382 | LATE_TIMEOUT |
| 2026-06-18T11:57:00Z | Sell | +7.0 → +4.0 | 0.2 | -0.3137 | CONVERGED |

## Differences from the primary run

| | Primary (Coinbase INTX) | This run (Kalshi) |
|---|---|---|
| Perp price | Trade candle open | Kalshi bid/ask at the minute open (mid) |
| Funding | Hourly, premium-TWAP forecast | Every 8 hours, last rate persisted |
| Entry check skipped without 60 min TWAP history | Yes | No (TWAP not used) |
| Candle timestamp | Start of minute | Kalshi `end_period_ts` shifted back 60s to the start |

## Realized accounting

`price_PnL = direction * q * [(perp_exit - perp_entry) - (spot_exit - spot_entry)]`

`funding_PnL = -direction * q * sum(recorded_mark * recorded_rate)`

Direction is +1 for buy basis and -1 for sell basis. Funding payments in
`(entry, exit]` count. Kalshi prices are quoted per 0.0001 BTC and are scaled
to USD per BTC. Trade bps divide net PnL by entry spot notional; their mean is
not an account return.

## Data and practical limits

Perp: Kalshi `KXBTCPERP` 1-minute bid/ask candles. Spot: Coinbase `BTC-USD`
minute candle opens, assumed executable with no spread. Funding: recorded
Kalshi 8-hour rates and marks. The Kalshi quote at the minute open and the
first spot trade of the minute are not exactly simultaneous. No fees, spot
spread, slippage, depth, borrow, margin, or liquidation is simulated; touch
fills assume the full size is available at the top of the book. Missing or
crossed Kalshi quotes are dropped, not forward-filled. This is a historical
zero-cost signal test, not locked arbitrage.

## Audit files

- [Trade ledger](trades.csv)
- [Minute mark-to-market equity](equity.csv)
- [Machine-readable summary](summary.json)
- [Vector chart](pnl.svg)
