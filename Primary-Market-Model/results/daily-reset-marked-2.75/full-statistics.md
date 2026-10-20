**Full statistics: 2.75 bp daily-reset strategy, marked day-end positions**

December 1, 2023–February 29, 2024 UTC; 91 days. Monetary PnL values are USDT. Net PnL includes all executed fees, funding, realized PnL and signed unrealized day-end PnL. Every trial starts flat; residual inventory is marked at last observed midprices without a terminal exit or closing fee, and does not carry into the next trial. These are separate trial valuations, not one continuous cash portfolio.

Daily performance and sensitivity to removing the five highest observed PnL days:

| Metric | All 91 days | Excluding the five best days |
| --- | --- | --- |
| Days | 91 | 86 |
| Total net PnL | +1,029.720337 | +236.318320 |
| Mean daily PnL | +11.315608 | +2.747887 |
| Median daily PnL | +0.262822 | +0.229566 |
| Sample standard deviation | 38.180052 | 10.593915 |
| Positive days | 62 | 57 |
| Negative days | 29 | 29 |
| Zero-PnL days | 0 | 0 |
| Positive-day rate | 68.13% | 66.28% |
| Best day | +219.721693 | +81.406735 |
| Worst day | -2.945086 | -2.945086 |
| Ordinary one-sided t statistic | 2.827236 | 2.405424 |
| Degrees of freedom | 90 | 85 |
| Ordinary one-sided t p | 0.00289280634 | 0.00916156659 |
| 95% two-sided mean interval | +3.364227 to +19.266989 | +0.476547 to +5.019227 |

The excluded days were selected after inspecting their PnL. The trimmed t-test/interval is a sensitivity diagnostic, not an independent confirmation. No HAC or bootstrap was computed by compressing the remaining gapped calendar.

| Additional full-sample daily metric | Value |
| --- | --- |
| Mean winning day | +16.899291 |
| Mean losing day | -0.621921 |
| Sum of positive daily PnL | +1,047.756060 |
| Sum of negative daily PnL | -18.035723 |
| Daily profit factor: positive-day sum / absolute negative-day sum | 58.093376 |
| Longest positive-day streak | 18 |
| Longest negative-day streak | 10 |
| Sample skewness | 3.952298 |
| Sample excess kurtosis | 15.552542 |
| Maximum drawdown of combined intraday marked curve | 18.300871 |
| Ordinary standard error of daily mean | 4.002357 |

| Daily PnL percentile | USDT |
| --- | --- |
| 5% | -1.071607 |
| 10% | -0.740210 |
| 25% | -0.087775 |
| 50% | +0.262822 |
| 75% | +1.426996 |
| 90% | +12.927903 |
| 95% | +94.008109 |

PnL accounting:

| Component | USDT |
| --- | --- |
| Price PnL before fees | +2,335.563512 |
| Executed fees | 1,318.237263 |
| Funding received | +12.394088 |
| Funding paid | 0.000000 |
| Net PnL | +1,029.720337 |
| Sum of realized PnL across trials | +1,043.837863 |
| Sum of signed unrealized endpoints | -14.117526 |

| Basis direction | Net PnL | Share of total | Funding | Executed fees |
| --- | --- | --- | --- | --- |
| Long perp / short spot | +434.045095 | 42.15% | +0.000000 | 468.686448 |
| Short perp / long spot | +595.675241 | 57.85% | +12.394088 | 849.550815 |

Monthly breakdown:

| Month | Net PnL | Long basis | Short basis | Fees | Funding | Positive days |
| --- | --- | --- | --- | --- | --- | --- |
| 2023-12 | +265.377850 | +124.488583 | +140.889268 | 262.082055 | +4.926785 | 26/31 |
| 2024-01 | +707.878141 | +301.392557 | +406.485584 | 917.701809 | +2.983040 | 21/31 |
| 2024-02 | +56.464345 | +8.163956 | +48.300389 | 138.453399 | +4.484262 | 15/29 |

Concentration of net PnL:

| Highest-earning days retained | Combined PnL | Share of total net PnL |
| --- | --- | --- |
| 1 | +219.721693 | 21.34% |
| 3 | +539.917027 | 52.43% |
| 5 | +793.402017 | 77.05% |
| 10 | +968.581572 | 94.06% |

The five best days and their activity. One trade opened is one engine position ID; partial fills share that ID. Each of these days has exactly one position still open at day-end.

| Date UTC | Net PnL | Basis trades opened | Completed positions | Paired partial fills |
| --- | --- | --- | --- | --- |
| 2024-01-09 | +219.721693 | 788 | 787 | 1136 |
| 2024-01-10 | +169.564155 | 816 | 815 | 1191 |
| 2024-01-03 | +150.631179 | 575 | 574 | 780 |
| 2023-12-11 | +146.875506 | 403 | 402 | 570 |
| 2024-01-05 | +106.609484 | 358 | 357 | 493 |

| Activity group | Trades opened | Days | Mean trades opened per day |
| --- | --- | --- | --- |
| Five best days | 2940 | 5 | 588.000000 |
| Other days | 2301 | 86 | 26.755814 |
| All days | 5241 | 91 | 57.593407 |

The five best days averaged 21.98 times as many trades as other days. Counting completed round trips only, the remaining 86-day average is 25.848837.

Next five best days:

| Date UTC | Net PnL |
| --- | --- |
| 2023-12-14 | +81.406735 |
| 2024-02-28 | +47.968525 |
| 2024-01-12 | +18.109572 |
| 2024-01-11 | +14.766820 |
| 2024-01-04 | +12.927903 |

Five worst days:

| Date UTC | Net PnL |
| --- | --- |
| 2024-01-26 | -2.945086 |
| 2024-01-31 | -2.388667 |
| 2024-01-29 | -1.593055 |
| 2024-02-05 | -1.290525 |
| 2024-02-08 | -1.087452 |

Statistical inference on full-sample daily endpoints. Every p-value below is one-sided for H0: mean PnL <= 0 versus H1: mean > 0; intervals are two-sided 95% intervals for mean USDT/day.

| Method | One-sided p | 95% lower bound | 95% upper bound |
| --- | --- | --- | --- |
| Ordinary one-sample t | 0.00289280634 | +3.364227 | +19.266989 |
| Newey–West HAC, 3 calendar-day lags | 0.0109931787 | +1.633363 | +20.997853 |
| Newey–West HAC, 7 calendar-day lags | 0.022281968 | +0.274805 | +22.356411 |
| Centered circular block bootstrap, 3-day blocks | 0.0180490975 | +0.850574 | +19.203529 |
| Centered circular block bootstrap, 7-day blocks | 0.0305984701 | -0.586350 | +20.398074 |

Bootstrap uses 20,000 resamples per block length. HAC uses Bartlett weights, n/(n−1) correction and an asymptotic normal reference; it is not an exact t distribution. One-sided p-values and two-sided intervals need not cross their significance thresholds together.

Adjacent-day Pearson correlation: 0.264926. Inventory resets do not establish independent market days. These tests use previously inspected history and do not account for strategy/threshold selection.

| Ljung–Box lags | Statistic | p |
| --- | --- | --- |
| 3 | 7.806903 | 0.0501756517 |
| 7 | 15.204940 | 0.0334602877 |
| 14 | 17.703383 | 0.220625975 |

| ACF lag in days | Demeaned sample autocorrelation |
| --- | --- |
| 1 | 0.264700 |
| 2 | 0.102343 |
| 3 | 0.048087 |
| 4 | 0.101807 |
| 5 | 0.096111 |
| 6 | 0.197371 |
| 7 | 0.126107 |
| 8 | -0.055045 |
| 9 | -0.061709 |
| 10 | -0.056445 |
| 11 | -0.070102 |
| 12 | -0.050780 |
| 13 | -0.057558 |
| 14 | -0.052942 |

Execution and position counts:

| Metric | Value |
| --- | --- |
| Basis positions opened | 5241 |
| Completed flat-to-flat positions | 5158 |
| Profitable completed positions | 3646 |
| Losing completed positions | 1512 |
| Completed-position win rate | 70.69% |
| Positions marked open at daily endpoints | 83 |
| Paired partial entry fills | 7597 |
| Closed paired partial lots | 7458 |
| Marked paired partial lots across all daily endpoints | 139 |
| Leg executions (entries and actual exits) | 30110 |
| Threshold signal episodes (not filled trades) | 190174 |
| Paired lots closed on opposite signals | 7456 |
| Paired lots closed before funding | 2 |
| Paired lots liquidated at day-end | 0 |
| Lifetime entered perp BTC | 46.726000 |
| Maximum concurrent perp BTC | 0.010000 |
| Maximum concurrent whole basis positions | 1 |
| Largest number of simultaneously open partial lots | 7 |

A paired partial lot is an immediately hedged partial fill, not a separate full basis trade. The seven-lot maximum is still at most 0.01 BTC total. Aggregate open-inventory fields describe only the final trial, not all historical marked endings.

| Execution type | Executed notional USDT | Fee bp | Fees USDT |
| --- | --- | --- | --- |
| Perp maker entry | 2,050,221.800800 | 0.5 | 102.511090 |
| Perp taker exit | 2,013,276.953700 | 2.0 | 402.655391 |
| Spot taker entry/exit | 4,065,353.908826 | 2.0 | 813.070782 |

Settings and data quality:

| Setting / check | Value |
| --- | --- |
| Equilibrium basis bp | -5.0 |
| Entry deviation bp | 2.75 |
| Long entry at/below bp | -7.75 |
| Short entry at/above bp | -2.25 |
| Basis trade BTC per leg | 0.01 |
| Maker fee bp | 0.5 |
| Taker fee bp per leg | 2.0 |
| Full entry/exit fee rate across four fills bp | 6.5 |
| Trade-print fill participation | 10.00% |
| Spot proxy half-spread bp | 0.5 |
| Latency ms | 0 |
| Funding-payer cancellation window seconds | 60.0 |
| Funding settlements independently reconciled | 273 |
| Side-attribution equity marks checked | 131519 |
| Maximum funding reconciliation error USDT | 8.87018791751e-11 |
| Maximum quote gap minutes | 43.132033 |
| Entry-band violations | 0 |
| Signed terminal marks verified against every daily pair ledger | True |

| UTC date with quote gap >10 seconds | Largest gap in minutes |
| --- | --- |
| 2023-12-07 | 7.831733 |
| 2024-01-03 | 13.773600 |
| 2024-02-28 | 43.132033 |

Stale books pause entries, but held positions remain marked at last observed references. Spot uses a last-trade proxy with assumed spread; maker fills use a simplified touch/participation model with zero latency. Queue position, impact, depth, borrowing costs, margin and liquidation are not modeled. Maximum drawdown and dollar PnL are model outputs, not percentage returns on a specified capital base.

Files: [extended statistics and all daily trade counts](extended-statistics.json), [daily PnL CSV](daily-pnl.csv), [full trial/inference JSON](trials.json), [execution summary](summary.json), [audits](validation.json), [source provenance](inputs.json).

