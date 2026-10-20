# Daily reset, marked endings: 1.25 bp

91 separate UTC trials, December 1, 2023 through February 29, 2024.
Equilibrium −5 bp; entry deviation 1.25 bp. One signed basis position
at most, 0.01 BTC per leg. Maker 0.5 bp, taker 2 bp per leg. Opposite-signal
and funding-payer exits remain enabled during each day.

Each trial starts flat with fresh state. At 23:59:59.999 UTC, remaining
positions are valued using the last observed perp and spot midprices.
Their signed unrealized gains AND losses are added to net PnL. No day-end
order, closing spread or exit fee is charged. Actual strategy exits still
pay their normal fees. Inventory is not carried into the next trial.

- Net marked PnL: -147.483861 USDT.
- Realized PnL sum: -128.193737 USDT.
- Unrealized endpoint PnL sum: -19.290125 USDT.
- Long basis net: +180.147659 USDT; short basis net: -327.631520 USDT.
- Executed fees: 3,015.022350 USDT. Funding received: 12.366918 USDT; paid zero.
- Positive trials: 31/91; mean -1.620702 USDT/day.
- One-sided daily one-sample t-test: t=-0.387049, df=90, p=0.650182998.
- 95% two-sided mean interval: [-9.939574, +6.698170] USDT/day.

Compared with the same daily trials liquidated at day-end: improvement
+16.687612 USDT = saved exit fees
+14.797289 + avoided crossing spread
+1.890322. Input archives, coverage,
entries, filled BTC, funding and intraday exits were independently matched
for all 91 days. 83 trials end with open inventory.

This is a sum of separate trial valuations, not a single executable
continuous cash portfolio: resetting a marked position does not physically
close it. Financial summary fields sum all daily endpoints; inventory
fields describe only the final trial. Daily resets do not guarantee market
independence. Lag-one correlation 0.378361; 7-lag HAC
one-sided p=0.590555554. Strategy selection is in-sample.
Spot uses last-trade proxies and an assumed spread; maker fills and latency
are simplified; borrowing costs and impact are omitted.

Charts: result.png (cumulative net by side), trials.png (daily marked net
PnL and one t interval). No separate unrealized chart or footer is shown.
Accounting: daily-pnl.csv, summary.json, validation.json, inputs.json.
Raw per-day ledgers and equity: ../../build/daily-reset-marked-1.25/days/.
Comparison chart: ../daily-reset-marked-comparison/comparison.png.

Reproduce from repository root:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_daily_archive_trials.py --entry-offset-bps 1.25 --close-at-day-end 0 --output Primary-Market-Model/build/daily-reset-marked-1.25 --report-dir Primary-Market-Model/results/daily-reset-marked-1.25
```
