# Minus/plus one basis-trade replay

The signed basis position is capped at -1/+1 trade. One trade is 0.01 BTC per leg. Partial fills may complete the position; opposite entries wait until the existing position closes. Entry events, fees, execution assumptions and funding-only exits match the preserved uncapped replay.

The run spans December 1, 2023 through February 29, 2024 UTC. Net marked PnL is **+17.97 USDT**: **+18.88 USDT funding**, plus **-0.91 USDT price PnL after fees**. Maximum observed position is **0.010 BTC per leg**, exactly one trade unit. There are no simultaneous opposite-direction positions.

One short-perp/long-spot position opened in three partial fills and remained open because it kept receiving funding. There were no long-side entries or funding exits. All 273 settlements reconcile. There are 85 positive daily equity changes out of 91, but those daily marks come from one carried position rather than 91 separate trades. The displayed one-sided t-test assumes independent daily observations.

The final week accounts for 17.23% of total marked PnL, versus 44.36% in the uncapped replay. The final day accounts for 6.14%. Position accumulation explains much of the uncapped late-period concentration; changing funding rates and price marks still affect the capped position.

`comparison.json` checks identical source manifests, event coverage, observation timestamps, fees and execution/exit settings. `validation.json` records the funding and position-cap checks. `result.png` and `trials.png` are 4K, footer-free presentation exports; the root results folder shows this run too. No terminal liquidation, borrowing, margin or market impact is modeled.
