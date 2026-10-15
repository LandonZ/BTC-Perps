#!/usr/bin/env python3
"""Plot the Kalshi basis backtest's mark-to-market PnL and write an auditable report.

Mirrors Primary-Market-Model/tools/plot_backtest.py for the Kalshi run's outputs.
"""
import argparse
import csv
import json
import os
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "btc-perps-mpl-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as dates
    import matplotlib.pyplot as plt

    summary = json.loads((args.directory / "summary.json").read_text())
    with (args.directory / "equity.csv").open(newline="") as file:
        equity = list(csv.DictReader(file))
    with (args.directory / "trades.csv").open(newline="") as file:
        trades = list(csv.DictReader(file))
    times = [datetime.fromtimestamp(int(row["ts"]), timezone.utc) for row in equity]
    net = [float(row["net_pnl"]) for row in equity]
    price = [float(row["price_pnl"]) for row in equity]
    funding = [float(row["funding_pnl"]) for row in equity]
    assert abs(net[-1] - summary["net_pnl"]) < 1e-7
    assert abs(sum(float(row["net_pnl"]) for row in trades) - summary["net_pnl"]) < 1e-7
    peak = 0.0
    drawdown = []
    for value in net:
        peak = max(peak, value)
        drawdown.append(value - peak)
    assert abs(-min(drawdown) - summary["max_drawdown"]) < 1e-7

    fill = summary["fill"]
    fill_text = ("bid/ask midpoint (idealized, like the primary run)" if fill == "mid"
                 else "buy at the ask, sell at the bid (pays the Kalshi spread)")
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(3, 1, figsize=(13, 9),
                             gridspec_kw={"height_ratios": [3, 1.1, 1.4]}, sharex=True)
    net_color = "#126c52" if net[-1] >= 0 else "#b12b36"
    axes[0].plot(times, price, color="#526ba6", lw=1.4, label="Basis-price PnL")
    axes[0].plot(times, funding, color="#b67d16", lw=1.6, label="Funding PnL")
    axes[0].plot(times, net, color=net_color, lw=2, label="Total PnL")
    axes[0].axhline(0, color="#adb5bd", lw=0.8)
    axes[0].set_ylabel("Cumulative PnL\nUSD")
    axes[0].legend(loc="upper left", frameon=False, ncols=3)
    axes[0].set_title(f"Kalshi KXBTCPERP basis — historical zero-cost backtest ({fill} fills)", loc="left",
                      fontsize=19, fontweight="bold", pad=33)
    rate = 100 * summary["winning_trades"] / summary["trades"] if summary["trades"] else 0
    subtitle = (f"{summary['quantity_btc']:g} BTC per leg  |  {summary['holding_minutes']:g}m cap  |  "
                f"{summary['trades']} trades  |  Net {net[-1]:+.2f}  |  Win rate {rate:.1f}%")
    axes[0].text(0, 1.035, subtitle, transform=axes[0].transAxes, color="#49545c", fontsize=11)
    axes[1].fill_between(times, drawdown, 0, color="#b12b36", alpha=0.24)
    axes[1].plot(times, drawdown, color="#b12b36", lw=0.8)
    axes[1].set_ylabel("Drawdown\nUSD")
    axes[1].text(0.99, 0.10, f"Maximum: {summary['max_drawdown']:.2f}",
                 transform=axes[1].transAxes, ha="right", color="#922c38")
    daily = defaultdict(float)
    for row in trades:
        day = datetime.fromtimestamp(int(row["exit_ts"]), timezone.utc).replace(hour=0, minute=0, second=0)
        daily[day] += float(row["net_pnl"])
    exits = sorted(daily)
    values = [daily[day] for day in exits]
    axes[2].bar(exits, values, width=0.7,
                color=["#126c52" if v >= 0 else "#b12b36" for v in values], alpha=0.85)
    axes[2].axhline(0, color="#adb5bd", lw=0.8)
    axes[2].set_ylabel("Daily realized PnL\nUSD")
    axes[2].xaxis.set_major_locator(dates.AutoDateLocator(minticks=5, maxticks=9))
    axes[2].xaxis.set_major_formatter(dates.ConciseDateFormatter(axes[2].xaxis.get_major_locator()))
    axes[2].set_xlabel("UTC date")
    for ax in axes:
        ax.grid(axis="y", alpha=0.17)
        ax.margins(x=0)
    fig.text(0.07, 0.02,
             f"Perp fills: {fill_text}. Spot: Coinbase BTC-USD minute opens. Recorded 8h Kalshi funding settles PnL.\n"
             "One pair at a time; zero fees. Missing deadline quotes exit at the next one. No market-maker fill simulation.",
             fontsize=9, color="#56616a")
    fig.subplots_adjust(left=0.10, right=0.97, top=0.87, bottom=0.13, hspace=0.25)
    fig.savefig(args.directory / "pnl.png", dpi=160)
    fig.savefig(args.directory / "pnl.svg")
    plt.close(fig)

    sides = []
    for name, label in (("LONG_PERP_SHORT_SPOT", "Buy basis"), ("SHORT_PERP_LONG_SPOT", "Sell basis")):
        rows = [row for row in trades if row["side"] == name]
        sides.append(f"| {label} | {len(rows)} | {sum(float(r['price_pnl']) for r in rows):+.6f} | "
                     f"{sum(float(r['funding_pnl']) for r in rows):+.6f} | "
                     f"{sum(float(r['net_pnl']) for r in rows):+.6f} |")
    threshold = summary["discount_threshold_bps"]
    target = summary.get("exit_target_bps", threshold)
    forced = sum(row["forced_end"] == "1" for row in trades)
    reason_counts = Counter(row["exit_reason"] for row in trades)
    reason_table = "\n".join(f"| {reason} | {count} |" for reason, count in sorted(reason_counts.items()))
    mean_hold = sum((int(r["exit_ts"]) - int(r["entry_ts"])) / 60 for r in trades) / len(trades) if trades else 0
    maximum_hold = max(((int(r["exit_ts"]) - int(r["entry_ts"])) / 60 for r in trades), default=0)
    mean_spread = sum(float(r["entry_spread_bps"]) for r in trades) / len(trades) if trades else 0
    funding_events = sum(int(row["funding_payments"]) for row in trades)
    funding_note = ("None of the measured trades crossed a funding boundary."
                    if funding_events == 0 else
                    f"The measured trades crossed {funding_events} 8-hour funding events; actual cash flows are included.")
    worst = sorted(trades, key=lambda r: float(r["net_pnl"]))[:5]
    worst_table = "\n".join(
        f"| {r['entry_utc']} | {'Buy' if r['side'].startswith('LONG') else 'Sell'} | "
        f"{float(r['entry_basis_bps']):+.1f} → {float(r['exit_basis_bps']):+.1f} | "
        f"{float(r['entry_spread_bps']):.1f} | {float(r['net_pnl']):+.4f} | {r['exit_reason']} |" for r in worst)
    assert all(int(r["exit_ts"]) - int(r["entry_ts"]) <= summary["holding_minutes"] * 60
               for r in trades if r["exit_reason"] != "LATE_TIMEOUT")

    report = f"""# Kalshi BTC basis backtest ({fill} fills)

**Total PnL: {summary['net_pnl']:+.6f} USD**, consisting of
{summary['price_pnl']:+.6f} basis-price PnL and {summary['funding_pnl']:+.6f} funding cash flows.

![Mark-to-market PnL](pnl.png)

| Metric | Result |
|---|---:|
| UTC data start | {summary['start_utc']} |
| UTC data end | {summary['end_utc']} |
| Perp fill price | {fill_text} |
| Entry threshold | ±{threshold:g} bps |
| Exit target | ±{target:g} bps |
| Exit on first profit | {'yes' if summary['exit_on_profit'] else 'no'} |
| Quantity per leg | {summary['quantity_btc']:g} BTC |
| Maximum holding period | {summary['holding_minutes']:g} minutes |
| Average measured holding period | {mean_hold:.3f} minutes |
| Longest measured trade | {maximum_hold:g} minutes |
| Mean Kalshi spread at entry | {mean_spread:.3f} bps |
| Trading/carry costs | 0 bps |
| Funding events crossed | {funding_events} |
| Trades | {summary['trades']} |
| Win rate | {rate:.2f}% |
| Mean trade PnL | {summary['mean_trade_bps']:+.6f} bps |
| Maximum observed minute mark-to-market drawdown | {summary['max_drawdown']:.6f} USD |
| Matched minute observations | {summary['matched_rows']:,} |
| Missing matched minutes inside covered span | {summary['missing_matched_minutes']:,} |
| Kalshi minutes dropped for empty/crossed quotes | {summary['bad_perp_quotes']} |
| Final pairs closed early at data end | {forced} |
| Late exits (deadline quote missing) | {summary['delayed_exits']} |

{funding_note}

| Direction | Trades | Basis-price PnL | Funding PnL | Total PnL |
|---|---:|---:|---:|---:|
{chr(10).join(sides)}

## Rule tested

The same rule as `Primary-Market-Model/results/basis-both-5m`, with the Kalshi
perp in place of Coinbase `BTC-PERP-INTX`. Check each available minute open, with
`basis = 10,000 * (perp / spot - 1)`:

- Buy basis: **long perp / short spot** when basis is below **-{threshold:g} bps**.
- Sell basis: **short perp / long spot** when basis is above **+{threshold:g} bps**.

Each entry also requires positive theo edge above {summary['minimum_edge_bps']:g} bps:
the basis move to the **-{target:g} / +{target:g} bps** exit target, minus funding the
side would pay if an 8-hour Kalshi funding boundary falls inside the
{summary['holding_minutes']:g}-minute window (the last recorded rate is persisted).

Close at the first available minute with positive total PnL (including funding)
{'when profit exits are enabled, as in this run' if summary['exit_on_profit'] else 'if profit exits are enabled; disabled in this run'},
when long basis reaches -{target:g} bps or higher or
short basis reaches +{target:g} bps or lower,
or at the {summary['holding_minutes']:g}-minute deadline.
Never close and reopen in the same minute. Hold one equal-BTC pair at a time.
No future prices select an entry. A final open pair is closed at data end.

If no matched price exists at the exact deadline, the pair closes at the next
observed minute (LATE_TIMEOUT). Trades are never dropped based on whether later
data exists, so the sample is not conditioned on future information.

| Exit reason | Trades |
|---|---:|
{reason_table}

### Worst trades

| Entry (UTC) | Side | Basis entry → exit (bps) | Entry spread (bps) | Net PnL | Exit |
|---|---|---|---:|---:|---|
{worst_table}

## Differences from the primary run

| | Primary (Coinbase INTX) | This run (Kalshi) |
|---|---|---|
| Perp price | Trade candle open | Kalshi bid/ask at the minute open ({fill}) |
| Funding | Hourly, premium-TWAP forecast | Every 8 hours, last rate persisted |
| Entry check skipped without 60 min TWAP history | Yes | No (TWAP not used) |
| Candle timestamp | Start of minute | Kalshi `end_period_ts` shifted back 60s to the start |

## Realized accounting

`price_PnL = direction * q * [(perp_exit - perp_entry) - (spot_exit - spot_entry)]`

`funding_PnL = -direction * q * sum(recorded_mark * recorded_rate)`

Direction is +1 for buy basis and -1 for sell basis. Funding payments in
`(entry, exit]` count. Kalshi prices are quoted per 0.0001 BTC and are scaled
to USD per BTC. Trade bps divide net PnL by entry spot notional; their mean is
not an account return.

## Data and practical limits

Perp: Kalshi `KXBTCPERP` 1-minute bid/ask candles. Spot: Coinbase `BTC-USD`
minute candle opens, assumed executable with no spread. Funding: recorded
Kalshi 8-hour rates and marks. The Kalshi quote at the minute open and the
first spot trade of the minute are not exactly simultaneous. No fees, spot
spread, slippage, depth, borrow, margin, or liquidation is simulated; touch
fills assume the full size is available at the top of the book. Missing or
crossed Kalshi quotes are dropped, not forward-filled. This is a historical
zero-cost signal test, not locked arbitrage.

## Audit files

- [Trade ledger](trades.csv)
- [Minute mark-to-market equity](equity.csv)
- [Machine-readable summary](summary.json)
- [Vector chart](pnl.svg)
"""
    (args.directory / "report.md").write_text(report)
    print(f"Report: {(args.directory / 'report.md').resolve()}")


if __name__ == "__main__":
    main()
