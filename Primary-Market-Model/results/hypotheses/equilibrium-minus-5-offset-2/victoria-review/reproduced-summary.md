# Basis strategy on recorded data

- **Coinbase BIP vs BTC-USD (2026-10-19, 14.8 h)**: basis mean +2.47 bp, median +2.65, std 1.76 (1-second grid, 40,778 fresh seconds).
- **Binance BTCUSDT perp vs spot (2026-10-01, 24.0 h)**: basis mean -4.69 bp, median -4.70, std 0.75 (1-second grid, 57,064 fresh seconds).

Net bps per round trip, zero latency (gross = before fees):

| venue | offset | trips | BTC | hold s | gross | 1 bp fees | 2 bp fees | 3 bp fees | 2 bp + 50 ms | 2 bp + 250 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| coinbase | ±0.5 | 174 | 1.74 | 34.34 | +1.28 | +0.28 | -0.71 | -1.72 | -1.20 | -2.06 |
| coinbase | ±1 | 82 | 0.82 | 65.16 | +1.78 | +0.78 | -0.22 | -1.22 | -0.64 | -1.69 |
| coinbase | ±1.5 | 50 | 0.5 | 99.51 | +2.19 | +1.19 | +0.19 | -0.81 | -0.30 | -1.46 |
| coinbase | ±2 | 29 | 0.29 | 158.47 | +2.64 | +1.64 | +0.64 | -0.36 | -0.06 | -1.48 |
| binance | ±0.5 | 54,609 | 198.091 | 17.17 | +0.96 | -0.04 | -1.04 | -2.04 | -1.12 | -1.27 |
| binance | ±1 | 23,006 | 83.06 | 22.14 | +1.41 | +0.41 | -0.59 | -1.59 | -0.80 | -1.00 |
| binance | ±1.5 | 8,685 | 30.845 | 27.61 | +1.89 | +0.89 | -0.11 | -1.11 | -0.51 | -0.79 |
| binance | ±2 | 1,757 | 6.675 | 31.59 | +2.78 | +1.78 | +0.78 | -0.22 | -0.06 | -0.50 |
