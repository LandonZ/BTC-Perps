# Late-February PnL concentration

Analysis of the saved final replay. No strategy settings, main results or presentation images were changed.

Late-February gains are funding carry on a large accumulated short-perp/long-spot position. February accounts for 80.9% of full-period marked PnL; the last seven days account for 44.4%, the last fourteen for 63.7%, and February 29 alone for 15.8%.

| UTC day | Funding (USDT) | Price PnL after fees (USDT) | Net marked PnL (USDT) | Sum of three funding rates (bp) |
|---|---:|---:|---:|---:|
| 2024-02-26 | +34,364.41 | +804.01 | +35,168.43 | 7.5555 |
| 2024-02-27 | +62,821.31 | -1,685.81 | +61,135.49 | 12.8748 |
| 2024-02-28 | +53,951.51 | -16,264.28 | +37,687.23 | 10.3949 |
| 2024-02-29 | +110,972.48 | -14,536.68 | +96,435.80 | 20.2459 |

The model retains receiving positions and permits multiple entries without a total inventory cap. End-of-day held BTC per leg grew from 64.516 on December 31 to 501.669 on January 31 and 873.366 on February 15. It was 874.562 on February 29. Thus late-month gains are not principally from an abrupt inventory increase in the final few days.

The February 29 rates were 6.9091, 7.6337 and 5.7031 bp, versus the standard flat 1 bp per settlement. The funding marks were approximately 62,478–62,809 USDT and the held short was about 874.52 BTC: roughly 55 million USDT of perp funding notional, matched with equal-BTC spot. Applying the archived rates produces 110,972.48 USDT of funding cash. Applying a hypothetical flat 1 bp to the same three settlement positions and marks would produce 16,444.98 USDT instead. This is a cash attribution holding the trade path fixed, not a counterfactual strategy replay.

Higher BTC prices increase dollar funding notional for the same BTC quantity. A common BTC price rise does not itself create price PnL on an equal-BTC hedge. Price PnL after fees was negative on the last three days.

Across all of February, average settlement exposure was about 800.30 BTC, versus 53.96 BTC in December. The average daily sum of funding rates was actually lower in February (4.51 bp) than December (5.44 bp), demonstrating the importance of growing exposure for the month-wide dollar comparison. The final few February days then combined that large exposure with unusually high rates.

| Replay cutoff (UTC, positions marked at mids) | Cumulative net PnL (USDT) |
|---|---:|
| February 15 | +221,268.47 |
| February 22 | +339,508.74 |
| February 28 | +513,712.20 |
| February 29 | +610,148.00 |

February 29 is an unusually large observation, and it has the highest daily sum of recorded funding rates in this sample. It is not the only source of positive PnL: the replay was already positive before the final week. These cutoff amounts are marked portfolio values along the same continuous path, with no liquidation at the cutoffs; they are not returns on equal capital.

Daily dollar PnL is not measured at constant exposure. Full-sample lag-one daily PnL correlation is approximately 0.569. The simple t-test shown in the presentation assumes independent observations; growth in inventory, changing funding regimes and dependence limit that interpretation. These post hoc cutoff checks are sensitivity diagnostics, not new independent evidence.

Source: final `trials.json` daily equity changes, `build/current/equity.csv` positions immediately before each settlement, and the local checksum-verified Binance funding and mark ZIPs referenced by `build/current/inputs.json`. Funding is checked as `-held_perp_btc * settlement_mark * recorded_rate`; all 273 payments reconcile in the replay validation. Full numbers are retained in `february-audit.json`.
