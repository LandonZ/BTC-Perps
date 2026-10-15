# Kalshi BTC basis backtest (touch fills)

**Total PnL: -102.331288 USD**, consisting of
-103.135600 basis-price PnL and +0.804312 funding cash flows.

![Mark-to-market PnL](pnl.png)

| Metric | Result |
|---|---:|
| UTC data start | 2026-06-03T14:02:00Z |
| UTC data end | 2026-10-12T20:07:00Z |
| Perp fill price | buy at the ask, sell at the bid (pays the Kalshi spread) |
| Quantity per leg | 0.01 BTC |
| Maximum holding period | 5 minutes |
| Average measured holding period | 2.510 minutes |
| Longest measured trade | 115 minutes |
| Mean Kalshi spread at entry | 1.442 bps |
| Trading/carry costs | 0 bps |
| Funding events crossed | 2 |
| Trades | 668 |
| Win rate | 89.37% |
| Mean trade PnL | -2.668181 bps |
| Maximum observed minute mark-to-market drawdown | 234.520000 USD |
| Matched minute observations | 186,667 |
| Missing matched minutes inside covered span | 2,339 |
| Kalshi minutes dropped for empty/crossed quotes | 52 |
| Final pairs closed early at data end | 0 |
| Late exits (deadline quote missing) | 6 |

The measured trades crossed 2 8-hour funding events; actual cash flows are included.

| Direction | Trades | Basis-price PnL | Funding PnL | Total PnL |
|---|---:|---:|---:|---:|
| Buy basis | 29 | +39.072900 | +0.000000 | +39.072900 |
| Sell basis | 639 | -142.208500 | +0.804312 | -141.404188 |

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
| LATE_TIMEOUT | 6 |
| PROFIT | 592 |
| TIMEOUT | 70 |

### Worst trades

| Entry (UTC) | Side | Basis entry → exit (bps) | Entry spread (bps) | Net PnL | Exit |
|---|---|---|---:|---:|---|
| 2026-06-05T02:49:00Z | Sell | +17.8 → +1966.5 | 11.7 | -122.1312 | TIMEOUT |
| 2026-06-05T02:55:00Z | Sell | +216.5 → +1670.8 | 221.5 | -90.9636 | TIMEOUT |
| 2026-10-01T07:50:00Z | Sell | +66.4 → +90.4 | 18.2 | -2.0080 | LATE_TIMEOUT |
| 2026-06-05T02:37:00Z | Sell | +7.0 → +26.3 | 2.1 | -1.2100 | TIMEOUT |
| 2026-06-05T03:28:00Z | Sell | +5.4 → +21.2 | 20.0 | -0.9974 | TIMEOUT |

## Differences from the primary run

| | Primary (Coinbase INTX) | This run (Kalshi) |
|---|---|---|
| Perp price | Trade candle open | Kalshi bid/ask at the minute open (touch) |
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
