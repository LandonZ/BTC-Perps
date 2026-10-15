# Kalshi results

Every run of the Kalshi `KXBTCPERP` vs Coinbase `BTC-USD` basis backtests and
maker simulation. PnL is in USD for 0.01 BTC per leg, with no trading fees unless
the run says otherwise. Each folder has `summary.json`, and most have `report.md`
and `pnl.png`. Rerun everything with `make everything` from `Kalshi-Market-Model`.

**Fills**: `mid` trades at the Kalshi mid (idealized). `touch` buys at the ask
and sells at the bid (pays the spread). Only touch results are realistic.

## The strategies

**Basis trade (taker)**: the headline runs and both sweeps. The Kalshi perp
should trade at about the same price as Coinbase spot. When the perp is more than
N bps above spot, sell the perp and buy the same amount of spot; when it is more
than N bps below, do the reverse. Either way you are hedged against BTC moving,
and you only bet that the gap closes. Close when the gap is back near zero, when
the trade is in profit (if profit exit is on), or when the max hold runs out. The
sweeps vary N (entry bps), the profit exit and the max hold.

**Market making (maker)**: `maker/`. Instead of paying the spread, rest a bid
and an ask on Kalshi a few bps either side of a fair value (recent average
perp/spot gap). When someone fills one of our quotes, immediately hedge on
Coinbase spot, then wait to close the perp back at fair value. The aim is to
earn the spread instead of paying it.

## Takeaways

- **The basis trade only works with an entry above the spread.** At touch fills,
  a 2 bps entry loses money, 3 bps barely makes any, and 5 to 8 bps is
  consistently profitable (about $70 to $180 from Jun 9).
- **Launch week loses money.** From launch, touch fills lose $102; from Jun 9
  they make $97. The early Kalshi book was too thin.
- **Holding longer helps a 3 bps entry**, from +$61 at 5 min to +$144 at 240 min,
  with fewer trades and a higher win rate. A 5 bps entry
  hardly changes with hold time.
- **Market making is all about latency.** With no latency it makes $1,473 in a
  week. At 0.5 s it makes $443, and at 1 s or more it loses: fills are adverse
  selection on stale quotes. Even top-tier fees turn the best case into −$4,727.

## Headline runs (`make report`)

5 bps entry, profit exit on, 5-minute max hold.

| Folder | Fills | Start | Trades | Win % | Net PnL | Max DD |
|---|---|---|---:|---:|---:|---:|
| `basis-both-5m` | mid | launch (Jun 3) | 1,111 | 96.8 | +$185.89 | $81.15 |
| `basis-both-5m-touch` | touch | launch (Jun 3) | 668 | 89.4 | −$102.33 | $234.52 |

## Exit sweep: `exit1-sweep/` (`make sweep-exit`)

Touch fills from 2026-06-09, exit at 1 bps, 5-minute max hold.

| Folder | Entry bps | Profit exit | Trades | Win % | Net PnL | Max DD |
|---|---:|---|---:|---:|---:|---:|
| `touch-entry3-profitexit0` | 3 | off | 7,283 | 47.4 | +$60.94 | $65.29 |
| `touch-entry3-profitexit1` | 3 | on | 9,911 | 75.7 | +$179.29 | $37.68 |
| `touch-entry5-profitexit0` | 5 | off | 446 | 70.4 | +$90.06 | $6.06 |
| `touch-entry5-profitexit1` | 5 | on | 580 | 90.3 | +$96.99 | $6.03 |
| `touch-entry8-profitexit0` | 8 | off | 50 | 78.0 | +$72.66 | $6.05 |
| `touch-entry8-profitexit1` | 8 | on | 58 | 84.5 | +$72.26 | $6.03 |

## Hold sweep: `exit1-hold/` (`make sweep-hold`)

Touch fills from 2026-06-09, exit at 1 bps, profit exit off. The 5-minute hold
is `exit1-sweep/touch-entry{3,5}-profitexit0`.

| Folder | Entry bps | Max hold | Trades | Win % | Net PnL | Max DD |
|---|---:|---:|---:|---:|---:|---:|
| `touch-entry3-hold15` | 3 | 15 min | 3,639 | 56.3 | +$120.04 | $18.77 |
| `touch-entry3-hold30` | 3 | 30 min | 2,287 | 61.6 | +$123.70 | $13.59 |
| `touch-entry3-hold60` | 3 | 60 min | 1,443 | 68.3 | +$125.80 | $7.70 |
| `touch-entry3-hold240` | 3 | 240 min | 636 | 83.3 | +$144.41 | $6.82 |
| `touch-entry5-hold15` | 5 | 15 min | 299 | 84.3 | +$93.10 | $6.11 |
| `touch-entry5-hold30` | 5 | 30 min | 234 | 90.6 | +$93.89 | $6.13 |
| `touch-entry5-hold60` | 5 | 60 min | 188 | 93.1 | +$94.01 | $6.07 |
| `touch-entry5-hold240` | 5 | 240 min | 128 | 96.9 | +$105.63 | $6.78 |

## Maker simulation: `maker/` (`make maker-report`)

Tick data, 2026-10-05 to 2026-10-12. The base case quotes ±2 bps around a
trailing 60-minute fair value, with 1 s quote and hedge latency, back of queue,
and no fees. Each run changes one thing. `maker/report.md` has the full table
and assumptions.

| Folder | Change from base | Trades | Win % | Net PnL | Max DD |
|---|---|---:|---:|---:|---:|
| `edge2` | none (base case) | 10,181 | 41.0 | −$516.39 | $516.69 |
| `edge3` | ±3 bps | 3,583 | 36.8 | −$285.18 | $285.33 |
| `edge5` | ±5 bps | 643 | 32.7 | −$99.56 | $100.01 |
| `fair-at-spot` | fair value = spot | 6,531 | 38.6 | −$355.35 | $356.25 |
| `edge2-queue-front` | front of queue | 11,828 | 46.6 | −$401.27 | $401.45 |
| `edge2-latency0` | no latency | 8,036 | 97.2 | +$1,472.62 | $1.52 |
| `edge2-latency0.5` | 0.5 s latency | 9,390 | 69.8 | +$443.35 | $29.54 |
| `edge2-latency2` | 2 s latency | 11,199 | 30.9 | −$888.39 | $888.51 |
| `edge2-latency0-fees-top-tier` | no latency, top-tier fees | 8,036 | 0.3 | −$4,727.19 | $4,727.19 |

### Mid-fill exit sweep (idealized fills)

From 2026-06-09, exit at 1 bps, 5-minute hold. Mid fills ignore the spread, so
these overstate PnL. Entry 2 bps shows +$1,051 at mid but loses at touch.

| Run | Entry bps | Profit exit | Trades | Win % | Net PnL | Max DD |
|---|---:|---|---:|---:|---:|---:|
| `mid-entry2-profitexit0` | 2 | off | 21,245 | 63.8 | +$510.26 | $6.57 |
| `mid-entry2-profitexit1` | 2 | on | 38,183 | 89.6 | +$1,050.64 | $6.57 |
| `mid-entry3-profitexit0` | 3 | off | 10,318 | 71.9 | +$400.15 | $3.23 |
| `mid-entry3-profitexit1` | 3 | on | 17,998 | 93.8 | +$689.98 | $3.23 |
| `mid-entry5-profitexit0` | 5 | off | 640 | 86.3 | +$125.15 | $3.25 |
| `mid-entry5-profitexit1` | 5 | on | 954 | 97.1 | +$139.84 | $3.23 |
| `mid-entry8-profitexit0` | 8 | off | 54 | 85.2 | +$91.89 | $0.35 |
| `mid-entry8-profitexit1` | 8 | on | 69 | 95.7 | +$91.79 | $0.20 |

```sh
./build/kalshi-backtester --start 2026-06-09 --fill mid --threshold-bps <entry> \
  --exit-bps 1 --profit-exit <0|1> --output results/exit1-sweep/mid-entry<entry>-profitexit<0|1>
```

### Touch fills with a 2 bps entry

A 2 bps entry sits inside the Kalshi spread, so every trade starts behind.

| Run | Profit exit | Trades | Win % | Net PnL | Max DD |
|---|---|---:|---:|---:|---:|
| `touch-entry2-profitexit0` | off | 17,593 | 35.6 | −$258.27 | $328.58 |
| `touch-entry2-profitexit1` | on | 23,667 | 64.9 | −$20.58 | $259.11 |

```sh
./build/kalshi-backtester --start 2026-06-09 --fill touch --threshold-bps 2 \
  --exit-bps 1 --profit-exit <0|1> --output results/exit1-sweep/touch-entry2-profitexit<0|1>
```

### Dominated maker runs

| Run | Why removed | Trades | Net PnL |
|---|---|---:|---:|
| `edge1` | ±1 bps: worse than `edge2` on every metric | 24,845 | −$997.66 |
| `edge2-latency0-fees-base-tier` | Base-tier fees (5/12 bps perp, 60 bps spot) are unrealistic; top tier already loses | 8,036 | −$86,091.95 |

```sh
./build/kalshi-maker --center-minutes 60 --edge-bps 1 --output results/maker/edge1
./build/kalshi-maker --center-minutes 60 --edge-bps 2 --quote-latency 0 --hedge-latency 0 \
  --perp-maker-fee-bps 5 --perp-taker-fee-bps 12 --spot-fee-bps 60 \
  --output results/maker/edge2-latency0-fees-base-tier
```

## Why each strategy works or doesn't

### Basis trade: works at touch with a 5 bps or wider entry, before fees

- **Why it makes money.** Kalshi is a small, new venue, and its perp is
  usually pushed *above* spot by people buying leveraged BTC (1,077 of the 1,111
  headline trades are shorts). Arbitrageurs pull it back within minutes, so a
  hedged bet that the gap closes usually wins (70 to 97% win rate at 5 bps).
- **Why the entry has to be wide.** The Kalshi spread averages about 3.5 bps.
  At touch you pay it on the way in and out, so a 2 bps entry starts every trade
  behind and loses. At 3 bps it only works if you hold long enough for the gap to
  fully close (hold sweep), and at 5 to 8 bps the gap is big enough to cover it.
- **Why launch week loses.** The book was thin in the first days, and on
  Jun 5 the perp blew out to almost +2,000 bps over spot. Two trades held into
  that lost about $213, more than every other trade made. A hedge does not help
  when the gap itself widens, so a tail like this is the real risk.
- **Why it may not survive real trading.** No fees are charged. At top-tier
  rates (2.6 bps Kalshi taker, 4 bps Coinbase spot, from `maker/report.md`) a
  round trip costs about 13 bps. The 5 bps runs average only 2 to 3 bps per
  trade, so fees wipe them out. Only the 8 bps entry (18 to 21 bps per trade)
  clears fees, and it trades about 50 times in four months. The backtest also
  assumes spot fills at the minute open with no spread, and that the full size is
  available at the Kalshi touch.

### Market making: doesn't work at realistic latency or fees

- **Why it looks good with no latency.** Quoting ±2 bps captures about 2 bps on
  each fill, and with an instant hedge that 2 bps is profit (97% win rate).
- **Why it loses with latency.** Our quotes follow Coinbase spot. When spot
  moves, a fast trader hits our quote before we update it, so we mostly get
  filled when our price is already stale. At 1 s latency, spot moves about
  2.15 bps against us before the hedge, more than the 1.97 bps we capture.
  Wider quotes (3 or 5 bps) trade less but still lose for the same reason.
- **Why fees end it anyway.** Even with no latency, top-tier fees (about
  0.6 bps Kalshi maker plus 4 bps Coinbase each way) are several times the 2 bps
  captured, turning +$1,473 into −$4,727. Making markets here would need
  sub-second latency and near-zero spot fees, for example hedging on a
  cheaper venue or not hedging every fill.
