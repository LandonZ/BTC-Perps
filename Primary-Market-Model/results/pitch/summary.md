# Basis strategy on recorded data

- **Coinbase BIP vs BTC-USD (2026-10-19, 10.7 h)**: basis mean +2.83 bp, median +3.42, std 1.71 (1-second grid, 33,264 fresh seconds).
- **Binance BTCUSDT perp vs spot (2026-10-01, 24.0 h)**: basis mean -4.69 bp, median -4.70, std 0.75 (1-second grid, 57,064 fresh seconds).

Net bps per round trip, zero latency (gross = before fees):

| venue | offset | trips | BTC | hold s | gross | 1 bp fees | 2 bp fees | 3 bp fees | 2 bp + 50 ms | 2 bp + 250 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| coinbase | ±0.5 | 153 | 1.53 | 6.85 | +1.26 | +0.26 | -0.74 | -1.74 | -1.24 | -2.13 |
| coinbase | ±1 | 71 | 0.71 | 7.49 | +1.75 | +0.75 | -0.25 | -1.25 | -0.74 | -1.77 |
| coinbase | ±1.5 | 41 | 0.41 | 4.44 | +2.20 | +1.20 | +0.20 | -0.80 | -0.38 | -1.51 |
| coinbase | ±2 | 23 | 0.23 | 2.79 | +2.70 | +1.70 | +0.70 | -0.30 | -0.18 | -1.41 |
| binance | ±0.5 | 54,609 | 198.091 | 17.17 | +0.96 | -0.04 | -1.04 | -2.04 | -1.12 | -1.27 |
| binance | ±1 | 23,006 | 83.06 | 22.14 | +1.41 | +0.41 | -0.59 | -1.59 | -0.80 | -1.00 |
| binance | ±1.5 | 8,685 | 30.845 | 27.61 | +1.89 | +0.89 | -0.11 | -1.11 | -0.51 | -0.79 |
| binance | ±2 | 1,757 | 6.675 | 31.59 | +2.78 | +1.78 | +0.78 | -0.22 | -0.06 | -0.50 |
