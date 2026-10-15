# Historical BTC basis backtest

**Total PnL: +52.116087 USDC equivalent**, consisting of
+51.184600 basis-price PnL and +0.931487 funding cash flows.

![Mark-to-market PnL](/Users/lucasbrown/Desktop/UT/Clubs/UCF/BTC-Perps/Primary-Market-Model/results/basis-both-5m/pnl.png)

| Metric | Result |
|---|---:|
| UTC data start | 2026-06-03T00:50:00Z |
| UTC data end | 2026-10-01T09:00:00Z |
| Quantity per leg | 0.01 BTC |
| Maximum holding period | 5 minutes |
| Average measured holding period | 2.439 minutes |
| Longest measured trade | 52 minutes |
| Trading/carry costs | 0 bps |
| Funding events crossed | 35 |
| Trades | 1052 |
| Win rate | 86.98% |
| Mean trade PnL | +0.663057 bps |
| Maximum observed minute mark-to-market drawdown | 0.909727 USDC equivalent |
| Matched minute observations | 148,446 |
| Missing matched minutes inside covered span | 24,845 |
| Entry checks skipped for missing features | 1250 |
| Exits delayed until next matched minute open | 9 |
| Final pairs closed early at data end | 0 |

The measured trades crossed 35 hourly funding events; actual cash flows are included.

| Direction | Trades | Basis-price PnL | Funding PnL | Total PnL |
|---|---:|---:|---:|---:|
| Buy basis | 23 | +5.300900 | +0.000000 | +5.300900 |
| Sell basis | 1029 | +45.883700 | +0.931487 | +46.815187 |

## Rule tested

Check each available minute open, with
`basis = 10,000 * (perp / spot - 1)`:

- Buy basis: **long perp / short spot** when basis is below **-5 bps**.
- Sell basis: **short perp / long spot** when basis is above **+5 bps**.

Each entry also requires positive theo edge above 0 bps
for that direction. The theo forecasts an exit basis of **-5 bps for longs / +5 bps for shorts** within
5 minutes. The past 60 completed candle closes proxy
the premium TWAP; only funding already recorded at entry supplies the prior rate.
The supplied smoothing formula forecasts funding payments inside that window.

Close at the first available minute with positive total PnL (including funding)
when profit exits are enabled, as in this run,
when long basis reaches -5 bps or higher or
short basis reaches +5 bps or lower,
or at the 5-minute deadline.
Never close and reopen in the same minute. Hold one equal-BTC pair at a time.
No future prices select an entry. A final open pair is closed at data end.

If no matched price exists at the exact deadline, the pair closes at the next
observed minute open (LATE_TIMEOUT). Trades are never dropped based on whether
later data exists, so the sample is not conditioned on future information.


| Exit reason | Trades |
|---|---:|
| LATE_TIMEOUT | 9 |
| PROFIT | 910 |
| TIMEOUT | 133 |

The funding forecast is `F_next = 0.75 * (TWAP_basis / 24) + 0.25 * F_previous`.
Its exit target is an assumption, not a guarantee that the basis converges.

## Realized accounting

`price_PnL = direction * q * [(perp_exit - perp_entry) - (spot_exit - spot_entry)]`

`funding_PnL = -direction * q * sum(recorded_mark * recorded_rate)`

Direction is +1 for buy basis and -1 for sell basis. Funding payments in
`(entry, exit]` count. Net PnL is their sum. Unlike the scenario calculator's
fixed spot forecast, realized PnL uses both markets' changing prices.

Trade bps divide each trade's net PnL by its entry spot notional. Their mean is
not an account return. No account capital, leverage return, CAGR, or compounding
is assumed. The price-only line uses the same positions with funding excluded.

## Data and practical limits

Prices: Coinbase `BTC-USD` spot and `BTC-PERP-INTX` perp minute candles. Funding:
the retained `BTC-PERP-INTX_funding_1h.csv`, with actual rates and funding marks.
Forecasts use the supplied formula; realized PnL uses recorded payments, not
those forecasts. Funding timestamps are assumed to be publication/settlement
times with no publication delay. Signal computation and execution latency are zero.

The funding premium proxy uses trade candle closes and BTC-USD, **not** Coinbase's
official per-second mark/index premium series. USD and USDC are assumed at parity.
Minute opens are idealized executable prices for both legs, not verified
simultaneous bids/asks. No maker queue, partial fills, fees, slippage, borrow
availability, financing, currency hedge, or liquidation is simulated. Missing
prices are not forward-filled; incomplete signal windows skip entries. Funding
must be complete hourly history. Drawdown is observed only at matched minute
opens. This is a historical zero-cost signal test, not locked arbitrage or proof
of achievable market-making profits.

## Audit files

- [Trade ledger](trades.csv)
- [Minute mark-to-market equity](equity.csv)
- [Machine-readable summary](summary.json)
- [Vector chart](pnl.svg)

Sources: [Coinbase candle API](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/public/get-public-product-candles)
and [original INTX rules](https://www.coinbase.com/international-exchange/legal/trading-rules).
