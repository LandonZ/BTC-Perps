# Daily reset, marked endings: 2.75 bp

91 separate UTC trials, December 1, 2023 through February 29, 2024.
Equilibrium −5 bp; entry deviation 2.75 bp. One signed basis position
at most, 0.01 BTC per leg. Maker 0.5 bp, taker 2 bp per leg. Opposite-signal
and funding-payer exits remain enabled during each day.

Each trial starts flat with fresh state. At 23:59:59.999 UTC, remaining
positions are valued using the last observed perp and spot midprices.
Their signed unrealized gains AND losses are added to net PnL. No day-end
order, closing spread or exit fee is charged. Actual strategy exits still
pay their normal fees. Inventory is not carried into the next trial.

- Net marked PnL: +1,029.720337 USDT.
- Realized PnL sum: +1,043.837863 USDT.
- Unrealized endpoint PnL sum: -14.117526 USDT.
- Long basis net: +434.045095 USDT; short basis net: +595.675241 USDT.
- Executed fees: 1,318.237263 USDT. Funding received: 12.394088 USDT; paid zero.
- Positive trials: 62/91; mean +11.315608 USDT/day.
- One-sided daily one-sample t-test: t=2.827236, df=90, p=0.00289280634.
- 95% two-sided mean interval: [+3.364227, +19.266989] USDT/day.

Compared with the same daily trials liquidated at day-end: improvement
+16.704370 USDT = saved exit fees
+14.812188 + avoided crossing spread
+1.892181. Input archives, coverage,
entries, filled BTC, funding and intraday exits were independently matched
for all 91 days. 83 trials end with open inventory.

This is a sum of separate trial valuations, not a single executable
continuous cash portfolio: resetting a marked position does not physically
close it. Financial summary fields sum all daily endpoints; inventory
fields describe only the final trial. Daily resets do not guarantee market
independence. Lag-one correlation 0.264926; 7-lag HAC
one-sided p=0.022281968. Strategy selection is in-sample.
Spot uses last-trade proxies and an assumed spread; maker fills and latency
are simplified; borrowing costs and impact are omitted.

Charts: result.png (cumulative net by side), trials.png (daily marked net
PnL and one t interval). No separate unrealized chart or footer is shown.
Accounting: daily-pnl.csv, summary.json, validation.json, inputs.json.
Raw per-day ledgers and equity: ../../build/daily-reset-marked-2.75/days/.
Comparison chart: ../daily-reset-marked-comparison/comparison.png.

Reproduce from repository root:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_daily_archive_trials.py --entry-offset-bps 2.75 --close-at-day-end 0 --output Primary-Market-Model/build/daily-reset-marked-2.75 --report-dir Primary-Market-Model/results/daily-reset-marked-2.75
```
