# Entry-band rerun and Victoria branch review

For a strategy that captures quick convergence back to the configured basis target,
Victoria's experiment addresses the right question and its Binance results
reproduce. Allen's historical profit is evidence of funding carry under the
replay's assumptions; it does not establish a profitable convergence strategy.
Neither experiment establishes live profitability or a universal −5 bp midpoint
equilibrium. The branches are not reporting conflicting calculations of the
same strategy.

Reviewed `victoria/basis_trade` at commit
[`b09498c3b7ec7cac8aa9ce780f5f56750887fe31`](https://github.com/LandonZ/BTC-Perps/tree/b09498c3b7ec7cac8aa9ce780f5f56750887fe31).
This was also the branch head returned by GitHub during the review. The branch
was read through GitHub's API into an isolated temporary directory. No Git
commands or checkout changes were used. The original 91-day `results/trials.json`
is byte-identical in both branches.

The new replay covers December 1, 2023 through February 29, 2024 UTC, with
entries at or below −7 bp / at or above −3 bp. All other settings match the
preserved 1.5 bp-offset baseline.

| Historical result (USDT unless stated) | Previous 1.5 bp offset | New 2 bp offset |
|---|---:|---:|
| Price PnL after fees | -4,573,129.29 | -4,321,854.22 |
| Funding cash | 83,698,980.63 | 81,449,722.19 |
| Net marked PnL | 79,125,851.33 | 77,127,867.97 |
| Maximum event-level drawdown | 144,075,526.51 | 139,072,001.85 |
| Entry partial-fill pairs | 15,092,093 | 14,577,432 |
| Peak gross BTC per leg | 82,499.347 | 79,721.452 |
| Final open BTC per leg | 82,499.327 | 79,721.432 |

The wider band reduces marked net PnL by 1,997,983.37 USDT
(-2.53%). Price PnL after fees improves by
251,275.07 USDT, while funding
receipts fall by 2,249,258.44 USDT. Inventory and
turnover also fall, so this dollar reduction alone does not establish that the
wider threshold is inferior on equal capital or risk.

The new run closes all 26,632 long-perp pairs before funding
and retains 14,550,800 short-perp pairs at the end; none exit on convergence.
It finishes with approximately 4.873 billion USDT of
spot exposure hedging the same BTC short in the perp. The approximately
139.07 million USDT event-level drawdown exceeds final profit;
quote gaps up to 43.1 minutes can distort interim
marks. These are simulated PnLs with unlimited inventory and no terminal
liquidation, not returns on a stated capital allocation.

All 273 funding settlements reconcile independently (largest error below
0.00000001 USDT); there are no paid-funding settlements in this sample. Entry-band
violations, convergence exits and net BTC delta are all zero. Direction
attributions reconcile at all 131,609 saved equity marks. Source hashes, event
coverage and observation clocks match the preserved baseline.

The 91 daily equity changes are positive on 85 days. The one-sided daily t-test
p-value is 7.87e-10; the 7-lag HAC p-value is 3.88e-5 and the centered 7-day
block-bootstrap p-value is 0.000200. Excluding funding along the same trade path,
the result is negative; the corresponding p-values are 0.810, 0.849 and 0.852.
These in-sample diagnostics do not establish an execution-realizable convergence
edge, and removing funding is an attribution rather than a different exit strategy.

The full report is [result.html](../result.html), with
[price curves](../result.png), [daily statistics](../trials.json),
[a baseline comparison chart](../comparison.png), and
[accounting validation](../validation.json).


| Assumption | Allen historical rerun | Victoria pitch sweep |
|---|---|---|
| Markets and dates | Binance BTCUSDT, Dec 1, 2023–Feb 29, 2024; 91 continuous days | Binance Oct 1, 2026; Coinbase BIP/BTC-USD Oct 19, 2026 |
| Entry target | Fixed −5 bp, offset 2 bp; long ≤ −7, short ≥ −3 | Binance: same target and offset in the 2 bp row; Coinbase: causal 10-minute EMA |
| Exit | Expected funding payers before settlement; receiving pairs remain open | Cross both spreads when basis returns to target; liquidate remaining pairs at end |
| Position limit | Unlimited concurrent inventory | 0.1 BTC gross perp; exits release capacity |
| Fee assumptions | Maker 0, takers 0.5 bp; nominal completed cycle 1.5 bp | Sweep of nominal cycle fees, with 2 bp emphasized; cycle fees split across four fills |
| Spot execution reference | Last observed spot trade with assumed 1 bp full spread | Observed spot bid and ask |
| Funding | Actual recorded funding on held inventory | Zero funding in the saved and reproduced pitch results |
| Latency | Zero | Zero-latency replay plus post hoc 50/250 ms repricing stresses |
| Reported outcome | Portfolio marked PnL, including open inventory and funding | Closed-pair PnL per entry spot notional, expressed in bp |

Dollar PnL between these experiments cannot be compared as a return on equal
capital. Inventory, date window, turnover, exit rules, fees and spot references
all differ. Equal BTC hedges common BTC price moves, while basis risk remains.

Victoria's complete Binance sweep reproduced exactly, including all saved
fee and latency rows. Selected **2 bp entry-offset** results are:

| Nominal completed-cycle fees | Hedge/exit repricing delay | Net bp per round trip | Net USDT |
|---:|---:|---:|---:|
| 2 bp | 0 ms | +0.777 | +43.66 |
| 2 bp | 50 ms | −0.063 | −3.52 |
| 2 bp | 250 ms | −0.500 | −28.07 |
| 1.5 bp | 0 ms | +1.277 | +71.76 |
| 1.5 bp | 50 ms | +0.437 | +24.58 |

These rows use 1,757 closed partial-fill pairs, totaling 6.675 BTC, with a mean
hold of 31.59 seconds. The recorded gross result is +2.777 bp. The sweep subtracts
a nominal fee amount from entry spot notional; the engine instead charges fees
on each executed fill's own notional. For example, the direct 2 bp fee replay
earns +43.67555 USDT rather than the sweep's rounded +43.66 USDT. This small
difference is explained by the normalization, not a PnL reconciliation failure.

To isolate the exit rule, I ran Victoria's engine on the **same Oct 1 Binance
input**, fixed −5 bp target, 2 bp offset, 0.1 BTC cap, 2 bp nominal cycle fees
(0.5 bp per fill), and zero latency:

| Exit treatment | Entry partial-fill pairs | Net USDT | Funding USDT | Final open BTC |
|---|---:|---:|---:|---:|
| Close at convergence; liquidate at end | 1,757 | +43.67555 | 0 | 0 |
| Funding-only exits; liquidate at end | 21 | +2.23172 | +0.97819 | 0 |
| Funding-only exits; mark remaining inventory | 21 | +3.08577 | +0.97819 | 0.1 |

With funding-only exits, the receiving position fills the capacity and prevents
the repeated turnover seen in the convergence strategy. This controlled check
demonstrates that the exit rule can explain large differences even on identical
data. It does not establish which exit rule performs better across other dates.

The branch's strongest supported conclusion is that the observed convergence
edge is small and sensitive to assumed execution costs. Several broader claims
need qualification:

- The saved Coinbase report covers 10.71 hours, but the committed Oct 19 recording
  now covers 14.85 hours. Rerunning the committed code/data gives 29 pairs at a
  2 bp offset, rather than the saved 23; the mean hold is 158.47 seconds rather
  than 2.79 seconds. Gross edge changes from +2.701 to +2.642 bp. Both the saved
  and reproduced summaries are retained here.
- One reproduced Coinbase pair lasts 2,167.486 seconds (36.1 minutes), from
  21:42:25.507 to 22:18:32.993 UTC, crossing the 22:00 funding boundary. Its
  recorded settlement rate is zero, so it pays no funding. Convergence exits
  alone do not guarantee that future trades avoid nonzero funding. Coinbase's
  sweep explicitly disables pre-funding exits and has no maximum holding time.
- The statement that 50 ms makes every configuration negative is conditional on
  the assumed 2 bp cycle fee. At Allen's nominal 1.5 bp cycle fee, Victoria's own
  Binance 2 bp-offset row remains positive at +0.437 bp under that stress.
- The statement that only a 2 bp offset is positive also needs venue/fee context:
  the saved Coinbase 1.5 bp-offset row is +0.197 bp at zero latency and 2 bp fees
  (the updated replay is approximately +0.19 bp).
- The latency rows reprice the hedge and both exits against later books. They
  preserve original signals, maker fills and decisions. They measure a particular
  execution stress, not the PnL of a full strategy replay with actual latency.
- A single day's basis mean near −5 bp and overlapping favorable markouts do not
  prove a stable price equilibrium or statistical significance. Under the
  configured funding formula, −5 bp is the zero-rate point for averaged premium;
  the entry signal instead uses instantaneous perp/spot midpoints. Those prices
  and sampling methods differ.

Victoria's data sample is too short to validate a live convergence edge. Allen's
larger historical sample has unlimited inventory, open positions marked at mids,
a spot trade-price proxy and material quote gaps. Neither replay models actual
queue position, market impact, borrowing, margin or liquidation. Assumed fee
tiers are experiment parameters rather than verified account pricing.

Both branches' regression suites passed. Allen's nine statistical-analysis tests
also passed, and direct boundary checks confirmed inclusive −7/−3 bp entries
with no fills just inside the band. Victoria's code and replay inputs were built
and run in isolation. Reproduced tables, source provenance, hashes and the
controlled-exit summaries are saved alongside this note in `evidence.json`,
`saved-sweep.csv`, `reproduced-sweep.csv`, and `controlled-exits.json`.

The appropriate conclusion for a quick basis-convergence thesis is Victoria's
qualified one: a small simulated gross edge can be consumed by execution costs.
Allen's positive historical total can support a different, funding-carry finding
under its assumptions. It should not be presented as confirmation of the quick
convergence thesis or as a realizable return on the required capital.
