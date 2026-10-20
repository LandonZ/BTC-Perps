# Continuous 2.75 bp: completed-trade t-test

December 1, 2023 through February 29, 2024 UTC. Equilibrium −5 bp,
long at/below −7.75 bp and short at/above −2.25 bp. Position −1/+1 trade,
0.01 BTC per leg; maker 0.5 bp, taker 2 bp per leg. Opposite-signal and
funding-payer exits retained. Inventory and funding history carry across
midnights. There are no daily or end-of-window forced exits.

Portfolio net PnL: +1,045.265658 USDT. Fees: 1,309.652960 USDT.
Funding received: 18.139562 USDT; paid: zero.
Long basis net: +434.002502 USDT;
short basis net: +611.263156 USDT.

## Event test

One observation is one completed flat-to-flat position. Its partial fills
share an engine position ID and are added together. Zero-fill threshold
signals are excluded. Each observation includes both legs' price PnL,
all executed entry/exit fees and its lifetime funding receipts/payments.

- Completed observations: 5,159 (3,653 profitable).
- Completed-trade total: +1,046.134254 USDT.
- Mean per completed trade: +0.202778 USDT.
- One-sided one-sample t-test: t=37.522881, df=5158, p=5.68585743e-273.
- 95% two-sided mean interval: [+0.192184, +0.213373] USDT/trade.
- Excluded terminal open positions: 1; marked contribution -0.868597 USDT.

The completed-trade total plus the terminal open contribution reconciles
to portfolio net PnL. No hypothetical closing fees are charged to positions
left open. The ordinary t-test is the requested primary display, with one
confidence interval and no footers or price-only dots.

Trades remain exposed to common market regimes. Lag-one trade correlation:
0.418175. Exit-day clustered one-sided p:
8.72718529e-13; seven-calendar-day HAC p:
1.57896261e-10. Those checks do not establish independence,
and exit-day clusters cannot capture every dependence channel in positions
held across several days. Thresholds were selected on previously inspected
history, so all tests are in-sample diagnostics. Spot is a last-trade proxy;
spread and maker fills are simplified. Borrow costs and impact are omitted.

## Files and reproduction

- result.png: cumulative net PnL by basis side.
- trials.png / trials.json: completed-trade PnL and t-test.
- events.csv: one row per completed trade.
- pairs.csv: partial-fill lot ledger, including position IDs and any open lots.
- daily-diagnostics.png / daily-diagnostics.json: daily equity-increment diagnostics.
- comparison.json: matched-data comparison against daily reset at 2.75 bp.
- validation.json / inputs.json: funding, position, accounting and source audits.

From repository root:

```bash
make -C Primary-Market-Model event-trials
```

Analyze an existing replay without another data pass:

```bash
Primary-Market-Model/build/trial-venv/bin/python Primary-Market-Model/tools/run_continuous_event_trials.py --analyze-only
```
