#!/usr/bin/env python3
"""Plot the C++ backtest's mark-to-market PnL and write an auditable report."""
import argparse
import csv
import json
import os
import sys
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--python-packages", type=Path,
                        help="Optional directory containing matplotlib and its dependencies")
    args = parser.parse_args()
    if args.python_packages:
        sys.path.insert(0, str(args.python_packages))
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "btc-perps-mpl-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as dates
    import matplotlib.pyplot as plt

    summary = json.loads((args.directory / "summary.json").read_text())
    strategy = summary.get("strategy", "theo_carry")
    threshold_strategy = strategy in ("long_basis_threshold", "short_basis_threshold", "both_basis_threshold")
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

    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(3, 1, figsize=(13, 9),
                             gridspec_kw={"height_ratios": [3, 1.1, 1.4]}, sharex=True)
    net_color = "#126c52" if net[-1] >= 0 else "#b12b36"
    # Keep all observations: the line includes open-position changes and funding payments.
    axes[0].plot(times, price, color="#526ba6", lw=1.4, label="Basis-price PnL")
    axes[0].plot(times, funding, color="#b67d16", lw=1.6, label="Funding PnL")
    axes[0].plot(times, net, color=net_color, lw=2, label="Total PnL")
    axes[0].axhline(0, color="#adb5bd", lw=0.8)
    axes[0].set_ylabel("Cumulative PnL\nUSDC equivalent")
    axes[0].legend(loc="upper left", frameon=False, ncols=3)
    axes[0].set_title("Coinbase BTC basis theo — historical zero-cost backtest", loc="left",
                      fontsize=19, fontweight="bold", pad=33)
    rate = 100 * summary["winning_trades"] / summary["trades"] if summary["trades"] else 0
    hold_text = f"{summary['holding_minutes']:g}m cap" if threshold_strategy else f"{summary['holding_minutes']/60:g}h hold"
    subtitle = (f"{summary['quantity_btc']:g} BTC per leg  |  {hold_text}  |  "
                f"{summary['trades']} trades  |  Net {net[-1]:+.2f}  |  Win rate {rate:.1f}%")
    axes[0].text(0, 1.035, subtitle, transform=axes[0].transAxes, color="#49545c", fontsize=11)
    axes[1].fill_between(times, drawdown, 0, color="#b12b36", alpha=0.24)
    axes[1].plot(times, drawdown, color="#b12b36", lw=0.8)
    axes[1].set_ylabel("Drawdown\nUSDC equivalent")
    axes[1].text(0.99, 0.10, f"Maximum: {summary['max_drawdown']:.2f}",
                 transform=axes[1].transAxes, ha="right", color="#922c38")
    if threshold_strategy:
        daily = defaultdict(float)
        for row in trades:
            day = datetime.fromtimestamp(int(row["exit_ts"]), timezone.utc).replace(hour=0, minute=0, second=0)
            daily[day] += float(row["net_pnl"])
        exits = sorted(daily)
        values = [daily[day] for day in exits]
        bar_width = 0.7
    else:
        exits = [datetime.fromtimestamp(int(row["exit_ts"]), timezone.utc) for row in trades]
        values = [float(row["net_bps"]) for row in trades]
        bar_width = max(summary["holding_minutes"] / 1440 * 0.65, 0.025)
    axes[2].bar(exits, values, width=bar_width,
                color=["#126c52" if v >= 0 else "#b12b36" for v in values], alpha=0.85)
    axes[2].axhline(0, color="#adb5bd", lw=0.8)
    axes[2].set_ylabel("Daily realized PnL\nUSDC equivalent" if threshold_strategy else "PnL per trade\nbps of entry spot notional")
    axes[2].xaxis.set_major_locator(dates.AutoDateLocator(minticks=5, maxticks=9))
    axes[2].xaxis.set_major_formatter(dates.ConciseDateFormatter(axes[2].xaxis.get_major_locator()))
    axes[2].set_xlabel("UTC date")
    for ax in axes:
        ax.grid(axis="y", alpha=0.17)
        ax.margins(x=0)
    fig.text(0.07, 0.02,
             "Past 60 candle closes proxy premium TWAP. Recorded funding settles PnL. "
             "USD/USDC assumed at parity.\n"
             "One pair at a time; idealized minute-open fills; zero costs. Missing deadline quotes exit at the next one. "
             "No market-maker fill simulation.", fontsize=9, color="#56616a")
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
    forced = sum(row["forced_end"] == "1" for row in trades)
    exit_forecast = summary["exit_basis_forecast"]
    exit_text = ("unchanged entry basis" if exit_forecast == "persist_entry_basis" else
                 f"-{summary['discount_threshold_bps']:g} bps for longs / +{summary['discount_threshold_bps']:g} bps for shorts"
                 if exit_forecast == "symmetric_threshold_targets" else f"{exit_forecast} bps")
    if threshold_strategy:
        entry_rules = []
        if strategy != "short_basis_threshold":
            entry_rules.append(f"- Buy basis: **long perp / short spot** when basis is below **-{summary['discount_threshold_bps']:g} bps**.")
        if strategy != "long_basis_threshold":
            entry_rules.append(f"- Sell basis: **short perp / long spot** when basis is above **+{summary['discount_threshold_bps']:g} bps**.")
        rule_text = f"""Check each available minute open, with
`basis = 10,000 * (perp / spot - 1)`:

{chr(10).join(entry_rules)}

Each entry also requires positive theo edge above {summary['minimum_edge_bps']:g} bps
for that direction. The theo forecasts an exit basis of **{exit_text}** within
{summary['holding_minutes']:g} minutes. The past 60 completed candle closes proxy
the premium TWAP; only funding already recorded at entry supplies the prior rate.
The supplied smoothing formula forecasts funding payments inside that window.

Close at the first available minute with positive total PnL (including funding)
{'when profit exits are enabled, as in this run' if summary['exit_on_profit'] else 'if profit exits are enabled; disabled in this run'},
when long basis reaches -{summary['discount_threshold_bps']:g} bps or higher or
short basis reaches +{summary['discount_threshold_bps']:g} bps or lower,
or at the {summary['holding_minutes']:g}-minute deadline.
Never close and reopen in the same minute. Hold one equal-BTC pair at a time.
No future prices select an entry. A final open pair is closed at data end.

If no matched price exists at the exact deadline, the pair closes at the next
observed minute open (LATE_TIMEOUT). Trades are never dropped based on whether
later data exists, so the sample is not conditioned on future information.
"""
    else:
        rule_text = f"""At each flat hourly boundary, use the previous 60 completed minute closes
to proxy premium TWAP and the latest funding rate already recorded. Project
the supplied funding rule across the holding period and forecast the exit
basis as **{exit_text}**. Select the side with positive theo edge, hold one
equal-BTC pair, and close at the holding horizon. The final pair closes at data end.
With an unchanged-basis forecast, this signal mainly forecasts funding carry.
"""
    reason_counts = Counter(row.get("exit_reason", "HORIZON") for row in trades)
    reason_table = "\n".join(f"| {reason} | {count} |" for reason, count in sorted(reason_counts.items()))
    mean_hold = sum((int(row["exit_ts"]) - int(row["entry_ts"])) / 60 for row in trades) / len(trades) if trades else 0
    maximum_hold = max(((int(row["exit_ts"]) - int(row["entry_ts"])) / 60 for row in trades), default=0)
    funding_events = sum(int(row["funding_payments"]) for row in trades)
    funding_note = ("None of the measured trades crossed a funding boundary. This run's gains therefore "
                    "come entirely from basis-price changes; it does not empirically validate the funding forecast."
                    if funding_events == 0 else
                    f"The measured trades crossed {funding_events} hourly funding events; actual cash flows are included.")
    if threshold_strategy:
        assert all(int(row["exit_ts"]) - int(row["entry_ts"]) <= summary["holding_minutes"] * 60
                   for row in trades if row["exit_reason"] != "LATE_TIMEOUT")
    report = f"""# Historical BTC basis backtest

**Total PnL: {summary['net_pnl']:+.6f} USDC equivalent**, consisting of
{summary['price_pnl']:+.6f} basis-price PnL and {summary['funding_pnl']:+.6f} funding cash flows.

![Mark-to-market PnL]({(args.directory / 'pnl.png').resolve()})

| Metric | Result |
|---|---:|
| UTC data start | {summary['start_utc']} |
| UTC data end | {summary['end_utc']} |
| Quantity per leg | {summary['quantity_btc']:g} BTC |
| Maximum holding period | {summary['holding_minutes']:g} minutes |
| Average measured holding period | {mean_hold:.3f} minutes |
| Longest measured trade | {maximum_hold:g} minutes |
| Trading/carry costs | 0 bps |
| Funding events crossed | {funding_events} |
| Trades | {summary['trades']} |
| Win rate | {rate:.2f}% |
| Mean trade PnL | {summary['mean_trade_bps']:+.6f} bps |
| Maximum observed minute mark-to-market drawdown | {summary['max_drawdown']:.6f} USDC equivalent |
| Matched minute observations | {summary['matched_rows']:,} |
| Missing matched minutes inside covered span | {summary['missing_matched_minutes']:,} |
| Entry checks skipped for missing features | {summary.get('skipped_feature_checks', summary['skipped_feature_hours'])} |
| Exits delayed until next matched minute open | {summary['delayed_exits']} |
| Final pairs closed early at data end | {forced} |

{funding_note}

| Direction | Trades | Basis-price PnL | Funding PnL | Total PnL |
|---|---:|---:|---:|---:|
{chr(10).join(sides)}

## Rule tested

{rule_text}

| Exit reason | Trades |
|---|---:|
{reason_table}

The funding forecast is `F_next = 0.75 * (TWAP_basis / 24) + 0.25 * F_previous`.
Its exit target is an assumption, not a guarantee that the basis converges.

## Realized accounting

`price_PnL = direction * q * [(perp_exit - perp_entry) - (spot_exit - spot_entry)]`

`funding_PnL = -direction * q * sum(recorded_mark * recorded_rate)`

Direction is +1 for buy basis and -1 for sell basis. Funding payments in
`(entry, exit]` count. Net PnL is their sum. Unlike the scenario calculator's
fixed spot forecast, realized PnL uses both markets' changing prices.

Trade bps divide each trade's net PnL by its entry spot notional. Their mean is
not an account return. No account capital, leverage return, CAGR, or compounding
is assumed. The price-only line uses the same positions with funding excluded.

## Data and practical limits

Prices: Coinbase `BTC-USD` spot and `BTC-PERP-INTX` perp minute candles. Funding:
the retained `BTC-PERP-INTX_funding_1h.csv`, with actual rates and funding marks.
Forecasts use the supplied formula; realized PnL uses recorded payments, not
those forecasts. Funding timestamps are assumed to be publication/settlement
times with no publication delay. Signal computation and execution latency are zero.

The funding premium proxy uses trade candle closes and BTC-USD, **not** Coinbase's
official per-second mark/index premium series. USD and USDC are assumed at parity.
Minute opens are idealized executable prices for both legs, not verified
simultaneous bids/asks. No maker queue, partial fills, fees, slippage, borrow
availability, financing, currency hedge, or liquidation is simulated. Missing
prices are not forward-filled; incomplete signal windows skip entries. Funding
must be complete hourly history. Drawdown is observed only at matched minute
opens. This is a historical zero-cost signal test, not locked arbitrage or proof
of achievable market-making profits.

## Audit files

- [Trade ledger](trades.csv)
- [Minute mark-to-market equity](equity.csv)
- [Machine-readable summary](summary.json)
- [Vector chart](pnl.svg)

Sources: [Coinbase candle API](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/public/get-public-product-candles)
and [original INTX rules](https://www.coinbase.com/international-exchange/legal/trading-rules).
"""
    (args.directory / "report.md").write_text(report)
    print(f"Chart: {(args.directory / 'pnl.png').resolve()}")
    print(f"Report: {(args.directory / 'report.md').resolve()}")


if __name__ == "__main__":
    main()
