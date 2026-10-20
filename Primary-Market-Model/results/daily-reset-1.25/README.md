# Daily-reset 1.25 bp experiment

91 separate UTC trials, December 1, 2023 through February 29, 2024.
Equilibrium −5 bp; long at/below −6.25 bp, short at/above −3.75 bp.
Position limit −1/+1 trade, 0.01 BTC per leg. Maker fee 0.5 bp; taker fee
2 bp per leg. Opposite-signal and funding-payer exits remain enabled.
Every trial starts flat with fresh state and closes remaining inventory
at day-end executable prices, charging 2 bp on each leg. All PnL is realized.

| Entry deviation | Net PnL (USDT) | Fees (USDT) | Positive days | One-sided t p |
| --- | ---: | ---: | ---: | ---: |
| 1.25 bp | -164.17 | 3,029.82 | 28/91 | 0.666272 |
| 2.75 bp | +1,013.02 | 1,333.05 | 51/91 | 0.00329231 |

Long basis net +180.15 USDT;
short basis net -344.32 USDT.
Funding received 12.37 USDT, paid zero.
Mean daily PnL -1.804082 USDT. One-sided one-sample t-test
t=-0.431060, df=90; 95% two-sided mean interval
[-10.118753, +6.510589] USDT/day.

The comparison verifies identical dates, input archives, replay binary,
feed coverage and all other settings. Net difference (1.25 minus 2.75 bp):
-1,177.19 USDT. The existing main presentation
images continue to show the 2.75 bp experiment.

Daily resets remove inventory carryover; market days may remain dependent.
Lag-one correlation 0.377992; 7-lag Newey-West one-sided
p=0.600649. Thresholds were selected on previously
inspected history. Results use a spot last-trade proxy, assumed spread,
zero latency and simplified maker fills. Borrow costs and impact are omitted.

Charts: result.png (cumulative PnL by side), trials.png (daily net PnL and
one t confidence interval), threshold-comparison.png (matched cumulative totals).
Daily accounting: daily-pnl.csv, daily-threshold-comparison.csv, monthly-pnl.csv.
Raw per-day summaries, equity and audits: ../../build/daily-reset-1.25/days/.

Reproduce from repository root:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_daily_archive_trials.py --entry-offset-bps 1.25 --output Primary-Market-Model/build/daily-reset-1.25 --report-dir Primary-Market-Model/results/daily-reset-1.25
```
