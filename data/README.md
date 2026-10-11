# Data

All market data for the BTC perp basis / market-making project. Everything comes from free, public, no-auth endpoints.

```bash
python3 data/fetch_data.py all                       # Kalshi + Coinbase + Binance, 2026-06-03 -> now
python3 data/fetch_data.py kalshi --trades-start 2026-09-01   # more tick history (~55k trades/day)
python3 data/fetch_data.py binance --start 2025-01-01          # longer primary-market history
python3 data/fetch_data.py record --seconds 86400 --interval 1 # live Kalshi L2 book recorder
```

The script uses only the Python stdlib. Every CSV has a header row, and its first column is `ts` in **unix seconds, UTC**, so the C++ code can read it with a plain `getline` + split. For candles, `ts` is the **start** of the minute on Coinbase/Binance and the **end** of the minute on Kalshi.

**Re-run `kalshi` at least weekly.** Kalshi keeps only about 7–10 days of the per-second premium index. Each run merges new points into the existing file, so the history keeps growing only if someone keeps pulling it.

## Kalshi — `KXBTCPERP` (target market) — `kalshi/`

Base URL `https://external-api.kalshi.com/trade-api/v2/margin` ([docs](https://docs.kalshi.com/margin)). The contract is **0.0001 BTC**, so a price of `8.3633` means $83,633/BTC. Tick size is 0.0001 (≈ 0.12 bps). The market launched on 2026-06-03.

| file | rows | what |
|---|---|---|
| `KXBTCPERP_funding.csv` | 3/day since launch | `funding_rate` (fraction per 8h) and `mark_price` at each funding time. Funding times are 04:00, 12:00 and 20:00 UTC |
| `KXBTCPERP_candles_1m.csv` | 1/min since launch | **bid and ask OHLC**, trade OHLC + mean, volume (contracts and USD), open interest, `mid_close_usd_per_btc` |
| `KXBTCPERP_premium_index_1s.csv` | 1/sec, rolling | Per-second premium index: (impact-price book − underlying index) / index. This is the basis signal |
| `trades/KXBTCPERP_trades_YYYY-MM-DD.csv` | every print | `price`, `count` (contracts), `taker_side` (`bid` = buyer aggressed), `trade_id` |
| `live/KXBTCPERP_book_*.csv` | from `record` | 10-level book snapshots + live premium index and estimated funding |
| `KXBTCPERP_market_info.json` | snapshot | contract specs, OI, 24h volume |

Notes:
- **Funding rule (checked against our data):** funding ≈ the time-weighted average of the premium index over the 8h window, and **it is set to 0 when |TWAP| < 1 bp**. 203 of the first 391 periods paid 0. The smallest nonzero |rate| is exactly 1.0 bp, and only 2 periods have been negative. This is not the Binance-style formula with the −5 bp x-intercept; see Binance below.
- In candles, empty fields and `0.0000` bids mean that side of the book was empty. This happens mostly in the first hours after launch. There are a few 2h gaps (exchange maintenance).
- Kalshi does not serve historical order-book depth. Only `record` captures depth, going forward.

## Coinbase — `coinbase/`

| file | source | notes |
|---|---|---|
| `BTC-USD_spot_1m.csv` | Advanced Trade public candles | **BTC spot reference**, OHLCV in USD |
| `BTC-PERP-INTX_1m.csv` | Advanced Trade `BTC-PERP-INTX` | Coinbase International perp (USDC) |
| `BTC-PERP-INTX_funding_1h.csv` | `api.international.coinbase.com/.../BTC-PERP/funding` | **Hourly** funding + mark price |
| `BIP-20DEC30-CDE_us_perp_1m.csv` | Advanced Trade `BIP-20DEC30-CDE` | Coinbase Derivatives US "perp-style" future, 0.01 BTC, hourly funding. Public API has candles only |

⚠️ **Coinbase International BTC-PERP has been `PAUSED` since 2026-10-01 09:00 UTC.** Its data stops there, so the INTX files cover 2026-06-03 → 2026-10-01. For a live Coinbase perp use BIP, or use Binance.

## Binance — `binance/` (deepest primary-market history)

Source: bulk archive at [data.binance.vision](https://data.binance.vision). The `fapi`/`api` REST endpoints are geo-blocked in the US, but the archive is not. Use `--start` to go back years.

| file | notes |
|---|---|
| `BTCUSDT_spot_klines_1m.csv` | spot |
| `BTCUSDT_perp_klines_1m.csv` | USDT-M perpetual |
| `BTCUSDT_perp_premium_index_1m.csv` | premium index OHLC per minute (the basis Binance funding is computed from) |
| `BTCUSDT_perp_mark_price_1m.csv` | mark price |
| `BTCUSDT_perp_funding.csv` | realized funding every 8h. Monthly archives only, so the current month appears after it ends |

Kline columns: `ts, open, high, low, close, volume, quote_volume, num_trades, taker_buy_base_volume, taker_buy_quote_volume`. For premium-index files, the OHLC values are premium fractions, not prices.

Binance funding is `F = P + clamp(0.01% − P, −0.05%, +0.05%)`, where P is the 8h average premium index. F = 0 at **P = −5 bp**. That is the −5 bp x-intercept in the pitch, so this formula comes from Binance, not Kalshi.

## Computing the basis

- **Kalshi:** use `premium_index_1s` directly (recent days only), or `mid_close_usd_per_btc / spot_close − 1` from the Kalshi candles joined to `coinbase/BTC-USD_spot_1m.csv`. Shift Kalshi `ts` by −60 to align bars by start time.
- **Binance:** `perp_klines.close / spot_klines.close − 1`, or use `perp_premium_index_1m` directly.
