# Daily-reset 2.75 bp experiment

91 separate UTC trials, December 1, 2023 through February 29, 2024.
Equilibrium −5 bp; long at/below −7.75 bp and short at/above −2.25 bp.
Maker 0.5 bp; taker 2 bp per fill. Position limit −1/+1 trade (0.01 BTC per leg).
Opposite-signal and funding-payer exits remain enabled. Every day starts flat
with a fresh engine; remaining positions close at day-end with 2 bp taker fees
on each leg. All 91 trials end with zero inventory and fully realized PnL.

Total net PnL +1013.015967 USDT. Funding received +12.394088 USDT, paid zero.
Execution fees 1333.049451 USDT. Positive trials 51/91. Mean 11.132044 USDT.
One-sided one-sample t-test t=2.781823, df=90, p=0.00329231;
95% two-sided t interval [3.181952, 19.082135] USDT per day.

Daily resets remove inventory carryover but do not establish statistical
independence: lag-one correlation 0.264747 and 7-lag Ljung-Box p=0.033786.
Newey-West 7-lag one-sided p=0.024020; 7-day block bootstrap p=0.032598.
Strategy and threshold were selected on previously inspected history, so all
inference is in-sample. Old and new experiments differ in both entry threshold
and trial horizon; the PnL difference cannot be attributed to either alone.

Raw per-day summaries, equity and audits: ../../build/daily-reset-2.75/days/.
Reproduce from repository root with:

    make -C Primary-Market-Model daily-trials
