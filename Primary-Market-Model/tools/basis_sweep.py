#!/usr/bin/env python3
"""Run the basis strategy on every recorded Coinbase and Binance day and chart the results.

For each venue, day and entry offset (fair basis +/- offset) this runs
build/basis-maker with Shreev's rules: rest a perp quote when the basis is far from
fair value, hedge each fill with an equal-BTC spot taker order, close the pair when
the basis returns to fair value, and close anything left at the end of the day.

Assumptions agreed with Shreev:
  * fees per fill: perp maker 0.5 bp, perp taker 2 bp, spot taker 2 bp, so one
    round trip (1 maker + 3 taker fills) costs 6.5 bp
  * zero latency: reversion around fair value plays out over hours, so acting now
    or a moment later makes no difference. --latencies 0,50,250 reprices the hedge
    and exit legs later as a sensitivity check.

Each day is replayed separately (flat start, flat end) and results are pooled per
venue and offset. The fee sweep restates net bps for other total cycle fees; fees
do not change any decision in the engine, so this equals rerunning it. Basis
markouts (how far the basis moves back toward fair value after a deviation) are
measured on a 1-second grid within each day and pooled.

Outputs go to results/pitch/: summary.md, sweep.csv, days.csv, markouts.csv and
PNG charts. Coinbase recordings are converted with tools/coinbase_basis_events.py
first if needed.

  make -C Primary-Market-Model build/basis-maker
  python3 Primary-Market-Model/tools/basis_sweep.py                      # every recorded day
  python3 Primary-Market-Model/tools/basis_sweep.py --binance-day 2026-10-01 --coinbase-day 2026-10-19
"""
import argparse
import json
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

from coinbase_basis_events import convert as convert_coinbase

MODEL = Path(__file__).resolve().parents[1]
ROOT = MODEL.parent
OFFSETS = [0.5, 1.0, 1.5, 2.0, 3.0, 4.0]
CYCLE_FEES = [0, 1, 2, 3, 3.5, 4, 5, 6, 6.5, 7]
HORIZONS = [1, 5, 10, 30, 60, 300, 900]
DEVIATIONS = [(0.5, 1.0), (1.0, 1.5), (1.5, 2.0), (2.0, 99)]
MIN_PLOT_HOURS = 2  # Coinbase recordings shorter than this are replayed but not drawn.

VENUES = {
    "coinbase": {
        "name": "Coinbase BIP vs BTC-USD",
        "center": "10-min EMA",
        # Fair basis is a 10-minute EMA: BIP has no -5 bp funding kink and trades ~3 bp rich.
        # BIP is 1-contract (0.01 BTC) on a $5 tick; trade sizes are whole contracts, so
        # participation is 100% of a print, but only prints THROUGH our price fill us.
        "args": ["--center-halflife-seconds", "600", "--exit-before-funding", "0", "--funding-cancel-seconds", "0",
                 "--tick", "5", "--quantity-step", "0.01", "--participation", "1", "--fill-at-touch", "0"],
    },
    "binance": {
        "name": "Binance BTCUSDT perp vs spot",
        "center": "−5 bp",
        # Fixed -5 bp equilibrium from Binance's funding formula; Allen's queue approximation.
        "args": ["--equilibrium-bps", "-5", "--participation", "0.1", "--fill-at-touch", "1"],
    },
}
COMMON = ["--exit-on-equilibrium", "1", "--close-at-end", "1", "--order-btc", "0.01",
          "--max-gross-perp-btc", "0.1", "--details", "1"]

# Reference palette (dataviz skill): surface, ink and the categorical slots in fixed order.
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#6250d6"]


def coinbase_days(days):
    """(day, events) for each Coinbase recording, converting it if needed."""
    out = []
    for day in days or sorted(p.name for p in (ROOT / "data/live").iterdir() if (p / "events.csv.gz").exists()):
        live = ROOT / "data/live" / day
        events = live / "basis-events.csv.gz"
        if not events.exists() or events.stat().st_mtime < (live / "events.csv.gz").stat().st_mtime:
            convert_coinbase(live, events)  # Missing, or the recording was updated since.
        out.append({"day": day, "events": events})
    return out


def binance_days(days):
    base = ROOT / "data/binance/tardis"
    out = []
    for day in days or sorted(p.name for p in base.iterdir() if (p / "events.csv.gz").exists()):
        spec = {"day": day, "events": base / day / "events.csv.gz", "premium": base / day / "premium-events.csv.gz"}
        for path in (spec["events"], spec["premium"]):
            if not path.exists():
                raise SystemExit(f"Missing {path}")
        out.append(spec)
    return out


def run_engine(venue, spec, offset, fees, out):
    cmd = [str(MODEL / "build/basis-maker"), "--events", str(spec["events"]), "--output", str(out),
           "--entry-offset-bps", str(offset), "--perp-maker-bps", str(fees[0]), "--perp-taker-bps", str(fees[1]),
           "--spot-taker-bps", str(fees[2]), *COMMON, *VENUES[venue]["args"]]
    if "premium" in spec:
        cmd += ["--premium-events", str(spec["premium"])]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    pairs = pd.read_csv(out / "pairs.csv")
    pairs = pairs[pairs.status == "CLOSED"].copy()
    summary = json.loads((out / "summary.json").read_text())
    if summary["open_pairs"]:
        raise SystemExit(f"{out}: {summary['open_pairs']} pairs left open despite --close-at-end")
    return pairs, summary


def books(events):
    """Perp (Q) and spot (S) top of book as time-indexed arrays."""
    e = pd.read_csv(events, usecols=["time_ms", "kind", "bid", "ask"])
    out = {}
    for kind, leg in (("Q", "perp"), ("S", "spot")):
        x = e[e.kind == kind].drop_duplicates("time_ms", keep="last")
        out[leg] = (x.time_ms.to_numpy(), x.bid.to_numpy(), x.ask.to_numpy())
    return out


def price_at(book, times, side_bid):
    t, bid, ask = book
    i = np.clip(np.searchsorted(t, times, side="right") - 1, 0, len(t) - 1)
    return np.where(side_bid, bid[i], ask[i])


def latency_change_usd(pairs, book, latency):
    """Change in pair PnL (USD) when the spot hedge and both exit legs act `latency` ms late."""
    if latency == 0 or pairs.empty:
        return np.zeros(len(pairs))
    d = np.where(pairs.direction == "LONG_PERP_SHORT_SPOT", 1, -1)
    long_ = d > 0
    entry_spot = price_at(book["spot"], pairs.time_ms.to_numpy() + latency, long_)     # long perp sells spot at the bid
    exit_perp = price_at(book["perp"], pairs.exit_time_ms.to_numpy() + latency, long_)  # and exits the perp at the bid
    exit_spot = price_at(book["spot"], pairs.exit_time_ms.to_numpy() + latency, ~long_)  # and buys spot back at the ask
    return (d * pairs.perp_btc * ((entry_spot - pairs.spot_price) + (exit_perp - pairs.exit_perp_price)
                                  - (exit_spot - pairs.exit_spot_price))).to_numpy()


def basis_grid(book, step=1000):
    t0 = max(book["perp"][0][0], book["spot"][0][0]) // step * step + step
    t1 = min(book["perp"][0][-1], book["spot"][0][-1])
    t = np.arange(t0, t1, step)
    mids, fresh = {}, np.ones(len(t), bool)
    for leg in ("perp", "spot"):
        times, bid, ask = book[leg]
        i = np.searchsorted(times, t, side="right") - 1
        mids[leg] = (bid[i] + ask[i]) / 2
        fresh &= (t - times[i]) < 10000
    basis = 1e4 * (mids["perp"] / mids["spot"] - 1)
    return pd.Series(np.where(fresh, basis, np.nan), index=pd.to_datetime(t, unit="ms", utc=True))


def markout_sums(venue, basis):
    """Per deviation bucket and horizon: (sum of moves toward fair value, count), within one day."""
    if venue == "binance":
        center = pd.Series(-5.0, index=basis.index)
    else:
        center = basis.ewm(halflife="600s", times=basis.index).mean().shift(1)
    b, dev = basis.to_numpy(), (basis - center).to_numpy()
    out = {}
    for lo, hi in DEVIATIONS:
        idx = np.where((np.abs(dev) >= lo) & (np.abs(dev) < hi))[0]
        for h in HORIZONS:
            j = idx[idx + h < len(b)]
            move = -np.sign(dev[j]) * (b[j + h] - b[j])
            move = move[np.isfinite(move)]
            out[(lo, hi, h)] = (move.sum(), len(move))
    return out


def sweep(venue_days, fees, latencies, work, jobs):
    """Run every venue/day/offset; returns (pair-level frame, per-day basis stats, markouts, Coinbase grid)."""
    tasks = [(venue, spec, offset) for venue, specs in venue_days.items() for spec in specs for offset in OFFSETS]

    def run(task):
        venue, spec, offset = task
        pairs, summary = run_engine(venue, spec, offset, fees, work / f"{venue}-{spec['day']}-offset-{offset}")
        pairs["venue"], pairs["day"], pairs["entry_offset_bps"] = venue, spec["day"], offset
        pairs["funding_usd_day"] = summary["funding_pnl"]
        return pairs

    with ThreadPoolExecutor(jobs) as pool:
        pairs = pd.concat(list(pool.map(run, tasks)), ignore_index=True)

    pairs["notional"] = pairs.perp_btc * pairs.spot_price
    pairs["fees_usd"] = pairs.entry_fees + pairs.exit_fees
    for latency in latencies:
        pairs[f"latency_{latency}_usd"] = 0.0
    day_stats, marks, coinbase_plot = [], {}, []
    for venue, specs in venue_days.items():
        for spec in specs:
            book = books(spec["events"])
            hours = (book["perp"][0][-1] - book["perp"][0][0]) / 3.6e6
            basis = basis_grid(book)
            day_stats.append({"venue": venue, "day": spec["day"], "hours": round(hours, 2),
                              "basis_mean": basis.mean(), "basis_median": basis.median(), "basis_std": basis.std(),
                              "basis_p05": basis.quantile(0.05), "basis_p95": basis.quantile(0.95),
                              "fresh_seconds": int(basis.notna().sum())})
            for key, (total, count) in markout_sums(venue, basis).items():
                prev = marks.get((venue, *key), (0.0, 0))
                marks[(venue, *key)] = (prev[0] + total, prev[1] + count)
            if venue == "coinbase" and hours >= MIN_PLOT_HOURS:
                coinbase_plot.append(basis)
            mask = (pairs.venue == venue) & (pairs.day == spec["day"])
            for latency in latencies:
                pairs.loc[mask, f"latency_{latency}_usd"] = latency_change_usd(pairs[mask], book, latency)
    markouts = pd.DataFrame([
        {"venue": venue, "deviation_bps": f"{lo:g}-{hi:g}" if hi < 99 else f">{lo:g}", "horizon_s": h,
         "seconds": count, "reversion_bps": round(total / count, 3) if count else np.nan}
        for (venue, lo, hi, h), (total, count) in marks.items()])
    coinbase_grid = pd.concat(coinbase_plot).sort_index() if coinbase_plot else None
    return pairs, pd.DataFrame(day_stats), markouts, coinbase_grid


def tables(pairs, day_stats, fees, latencies):
    """Pooled results per venue/offset (with fee and latency variants) and per day."""
    cycle = fees[0] + fees[1] + 2 * fees[2]
    rows = []
    for (venue, offset), g in pairs.groupby(["venue", "entry_offset_bps"], sort=False):
        notional = g.notional.sum()
        hours = day_stats[day_stats.venue == venue].hours.sum()
        net = g.realized_pnl.sum()
        gross = net + g.fees_usd.sum()
        base = {"venue": venue, "entry_offset_bps": offset, "days": g.day.nunique(), "hours": round(hours, 2),
                "round_trips": len(g), "btc_traded": round(g.perp_btc.sum(), 4),
                "median_hold_s": round(float((g.exit_time_ms - g.time_ms).median() / 1000), 2) if len(g) else 0,
                "max_hold_s": round(float((g.exit_time_ms - g.time_ms).max() / 1000), 1) if len(g) else 0,
                "funding_usd": round(float(g.funding_pnl.sum()), 4),
                "gross_bps": round(gross / notional * 1e4, 3) if notional else np.nan,
                "net_bps": round(net / notional * 1e4, 3) if notional else np.nan,
                "net_usd": round(net, 2), "cycle_fee_bps": cycle}
        for fee in CYCLE_FEES:
            base[f"net_bps_at_{fee:g}_fee"] = round((gross / notional * 1e4) - fee, 3) if notional else np.nan
        for latency in latencies:
            if latency:
                change = g[f"latency_{latency}_usd"].sum()
                base[f"net_bps_{latency}ms"] = round((net + change) / notional * 1e4, 3) if notional else np.nan
        rows.append(base)
    days = (pairs.groupby(["venue", "day", "entry_offset_bps"])
            .agg(round_trips=("realized_pnl", "size"), net_usd=("realized_pnl", "sum"), fees_usd=("fees_usd", "sum"),
                 notional=("notional", "sum"))
            .reset_index())
    days["gross_bps"] = ((days.net_usd + days.fees_usd) / days.notional * 1e4).round(3)
    days["net_bps"] = (days.net_usd / days.notional * 1e4).round(3)
    days = days.merge(day_stats, on=["venue", "day"], how="right").sort_values(["venue", "day", "entry_offset_bps"])
    return pd.DataFrame(rows), days


def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=12, pad=10)
    ax.set_xlabel(xlabel, color=INK2)
    ax.set_ylabel(ylabel, color=INK2)
    ax.tick_params(colors=INK2, length=0)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left", "bottom"):
        ax.spines[side].set_visible(False)


def bottom_legend(fig, ax, title, ncol=None):
    handles, labels = ax.get_legend_handles_labels()
    fig.tight_layout(rect=(0, 0.14, 1, 1))
    fig.legend(handles, labels, loc="lower center", ncol=ncol or len(labels), frameon=False, labelcolor=INK2,
               fontsize=9, title=title, title_fontsize=9)


def charts(results, days, markouts, coinbase_grid, fees, latencies, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"figure.facecolor": SURFACE, "font.size": 10, "lines.linewidth": 2})
    cycle = fees[0] + fees[1] + 2 * fees[2]
    venues = list(results.venue.unique())
    stats = days.drop_duplicates(["venue", "day"])

    # 1. Binance: where the basis sat on each recorded day, against the -5 bp equilibrium.
    b = stats[stats.venue == "binance"]
    if len(b):
        fig, ax = plt.subplots(figsize=(9, 3.6))
        x = np.arange(len(b))
        ax.vlines(x, b.basis_p05, b.basis_p95, color=SERIES[0], linewidth=2, alpha=0.45)
        ax.plot(x, b.basis_mean, "o", color=SERIES[0], markersize=8)
        ax.axhline(-5, color=INK2, linewidth=1, linestyle="--")
        ax.set_xticks(x, [d[:7] for d in b.day], rotation=45)
        ax.text(0.01, 0.04, f"dot: daily mean · bar: 5th-95th percentile\nmean of daily means {b.basis_mean.mean():+.2f} bp"
                "\ndashed: −5 bp funding equilibrium", transform=ax.transAxes, color=INK, fontsize=9, va="bottom",
                bbox={"facecolor": SURFACE, "edgecolor": GRID, "boxstyle": "round,pad=0.4"})
        style(ax, f"Binance perp − spot basis on the first day of each month ({len(b)} days)", "", "basis (bp)")
        fig.tight_layout()
        fig.savefig(out / "basis-binance.png", dpi=160)
        plt.close(fig)

    # 2. Coinbase: the basis over the longer recordings; its fair value drifts.
    if coinbase_grid is not None:
        fig, ax = plt.subplots(figsize=(9, 3.6))
        minute = coinbase_grid.resample("1min").mean()
        ax.plot(minute.index, minute.values, color=SERIES[0], linewidth=1.5)
        ax.text(0.01, 0.04, f"mean {coinbase_grid.mean():+.2f} bp · median {coinbase_grid.median():+.2f} bp"
                "\nBIP trades rich to spot; fair value drifts", transform=ax.transAxes, color=INK, fontsize=9,
                va="bottom", bbox={"facecolor": SURFACE, "edgecolor": GRID, "boxstyle": "round,pad=0.4"})
        hours = stats[(stats.venue == "coinbase") & (stats.hours >= MIN_PLOT_HOURS)].hours.sum()
        style(ax, f"Coinbase BIP − BTC-USD basis, 1-minute mean ({hours:.1f} h recorded)", "UTC", "basis (bp)")
        fig.autofmt_xdate()
        fig.tight_layout()
        fig.savefig(out / "basis-coinbase.png", dpi=160)
        plt.close(fig)

    # 3. Markouts: how much of a deviation reverts, and how fast.
    fig, axes = plt.subplots(1, len(venues), figsize=(5 * len(venues), 3.8), sharey=True, squeeze=False)
    for ax, venue in zip(axes[0], venues):
        m = markouts[markouts.venue == venue]
        for color, (bucket, g) in zip(SERIES, m.groupby("deviation_bps", sort=False)):
            ax.plot(g.horizon_s, g.reversion_bps, color=color, marker="o", markersize=4, label=f"{bucket} bp off")
        ax.set_xscale("log")
        ax.axhline(0, color=INK2, linewidth=1)
        style(ax, f"{venue.capitalize()}: reversion toward {VENUES[venue]['center']}", "seconds after deviation",
              "basis move toward fair (bp)")
    bottom_legend(fig, axes[0][0], "deviation from fair value")
    fig.savefig(out / "markouts.png", dpi=160)
    plt.close(fig)

    # 4. Fees decide it: net bps per round trip vs total cycle fee.
    fig, axes = plt.subplots(1, len(venues), figsize=(5 * len(venues), 4.2), sharey=True, squeeze=False)
    for ax, venue in zip(axes[0], venues):
        r = results[results.venue == venue]
        for color, (_, g) in zip(SERIES, r.groupby("entry_offset_bps")):
            ax.plot(CYCLE_FEES, [g[f"net_bps_at_{f:g}_fee"].iloc[0] for f in CYCLE_FEES], color=color, marker="o",
                    markersize=4, label=f"±{g.entry_offset_bps.iloc[0]:g} bp")
        ax.axhline(0, color=INK2, linewidth=1)
        ax.axvline(cycle, color=INK2, linewidth=1, linestyle="--")
        ax.annotate(f"our fees: {cycle:g} bp", (cycle, 1), xycoords=("data", "axes fraction"), xytext=(4, -12),
                    textcoords="offset points", color=INK2, fontsize=9)
        style(ax, f"{venue.capitalize()}: net per round trip", "total fees per round trip (bp)", "net bp per round trip")
    bottom_legend(fig, axes[0][0], "enter when the basis is this far from fair value")
    fig.savefig(out / "fees.png", dpi=160)
    plt.close(fig)

    # 5. Trade-off at our fees: wider entries earn more per trip but trade less.
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), squeeze=False)
    for color, venue in zip(SERIES, venues):
        r = results[results.venue == venue]
        axes[0][0].plot(r.entry_offset_bps, r.net_bps, color=color, marker="o", markersize=5, label=venue.capitalize())
        axes[0][1].plot(r.entry_offset_bps, r.round_trips / r.hours * 24, color=color, marker="o", markersize=5)
    axes[0][0].axhline(0, color=INK2, linewidth=1)
    style(axes[0][0], f"Net per round trip at {cycle:g} bp fees", "entry offset from fair (bp)", "net bp per round trip")
    style(axes[0][1], "Round trips per 24 h", "entry offset from fair (bp)", "round trips (log scale)")
    axes[0][1].set_yscale("log")
    bottom_legend(fig, axes[0][0], "")
    fig.savefig(out / "offsets.png", dpi=160)
    plt.close(fig)

    # 6. Binance day by day at our fees, for the wider entries.
    d = days[(days.venue == "binance") & (days.entry_offset_bps >= 2)]
    if len(d):
        fig, ax = plt.subplots(figsize=(9, 3.8))
        day_list = sorted(d.day.unique())
        x = np.arange(len(day_list))
        offsets = sorted(d.entry_offset_bps.unique())
        width = 0.8 / len(offsets)
        for k, (color, offset) in enumerate(zip(SERIES[3:], offsets)):
            g = d[d.entry_offset_bps == offset].set_index("day").reindex(day_list)
            ax.bar(x + (k - (len(offsets) - 1) / 2) * width, g.net_usd.fillna(0), width * 0.9, color=color,
                   label=f"±{offset:g} bp")
        ax.axhline(0, color=INK2, linewidth=1)
        ax.set_xticks(x, [day[:7] for day in day_list], rotation=45)
        style(ax, f"Binance net USD per day at {cycle:g} bp fees (0.01 BTC lots, ≤0.1 BTC open)", "", "net USD")
        bottom_legend(fig, ax, "entry offset")
        fig.savefig(out / "binance-days.png", dpi=160)
        plt.close(fig)

    # Optional: what acting late costs.
    if any(latencies):
        fig, axes = plt.subplots(1, len(venues), figsize=(5 * len(venues), 3.8), sharey=True, squeeze=False)
        for ax, venue in zip(axes[0], venues):
            r = results[results.venue == venue]
            for color, latency in zip(SERIES, latencies):
                col = "net_bps" if latency == 0 else f"net_bps_{latency}ms"
                ax.plot(r.entry_offset_bps, r[col], color=color, marker="o", markersize=4, label=f"{latency} ms")
            ax.axhline(0, color=INK2, linewidth=1)
            style(ax, f"{venue.capitalize()}: net vs hedge delay", "entry offset from fair (bp)", "net bp per round trip")
        bottom_legend(fig, axes[0][0], "hedge and exit delay")
        fig.savefig(out / "latency.png", dpi=160)
        plt.close(fig)


def summary_md(results, days, fees, latencies):
    cycle = fees[0] + fees[1] + 2 * fees[2]
    stats = days.drop_duplicates(["venue", "day"])
    lines = ["# Basis strategy on recorded data", "",
             f"Fees per fill: perp maker {fees[0]:g} bp, perp taker {fees[1]:g} bp, spot taker {fees[2]:g} bp "
             f"({cycle:g} bp per round trip). Zero latency. 0.01 BTC lots, at most 0.1 BTC open. "
             "Each day starts and ends flat.", ""]
    for venue in results.venue.unique():
        s = stats[stats.venue == venue]
        lines.append(f"- **{VENUES[venue]['name']}**: {len(s)} days, {s.hours.sum():.1f} h. Daily mean basis "
                     f"{s.basis_mean.min():+.2f} to {s.basis_mean.max():+.2f} bp (average {s.basis_mean.mean():+.2f}); "
                     f"typical within-day std {s.basis_std.median():.2f} bp.")
    extra = [f"{lat} ms" for lat in latencies if lat]
    lines += ["", "Pooled over all days (gross = before fees):", "",
              "| venue | offset | trips | trips/24h | BTC | median hold s | gross bp | net bp | net USD | "
              + " | ".join(["net bp @2 fee", "net bp @3.5 fee"] + [f"net bp +{e}" for e in extra]) + " |",
              "|" + "---|" * (11 + len(extra))]
    for _, r in results.iterrows():
        cells = [r.venue, f"±{r.entry_offset_bps:g}", f"{r.round_trips:,}", f"{r.round_trips / r.hours * 24:,.0f}",
                 f"{r.btc_traded:g}", f"{r.median_hold_s:g}", f"{r.gross_bps:+.2f}", f"{r.net_bps:+.2f}",
                 f"{r.net_usd:+,.2f}", f"{r['net_bps_at_2_fee']:+.2f}", f"{r['net_bps_at_3.5_fee']:+.2f}"]
        cells += [f"{r[f'net_bps_{lat}ms']:+.2f}" for lat in latencies if lat]
        lines.append("| " + " | ".join(cells) + " |")
    lines += ["", "Per day (net USD at our fees):", "",
              "| venue | day | hours | mean basis | " + " | ".join(f"±{o:g}" for o in OFFSETS) + " |",
              "|" + "---|" * (4 + len(OFFSETS))]
    for (venue, day), g in days.groupby(["venue", "day"], sort=False):
        by = g.set_index("entry_offset_bps").net_usd
        lines.append(f"| {venue} | {day} | {g.hours.iloc[0]:.1f} | {g.basis_mean.iloc[0]:+.2f} | "
                     + " | ".join(f"{by.get(o, 0):+,.2f}" for o in OFFSETS) + " |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=MODEL / "results/pitch")
    parser.add_argument("--work", type=Path, default=MODEL / "build/sweep")
    parser.add_argument("--coinbase-day", action="append", help="recording under data/live/ (repeatable; default all)")
    parser.add_argument("--binance-day", action="append", help="day under data/binance/tardis/ (repeatable; default all)")
    parser.add_argument("--perp-maker-bps", type=float, default=0.5)
    parser.add_argument("--perp-taker-bps", type=float, default=2)
    parser.add_argument("--spot-taker-bps", type=float, default=2)
    parser.add_argument("--latencies", default="0", help="comma-separated ms, e.g. 0,50,250 (default 0)")
    parser.add_argument("--jobs", type=int, default=4, help="engine runs in parallel")
    parser.add_argument("--no-charts", action="store_true")
    args = parser.parse_args()
    fees = (args.perp_maker_bps, args.perp_taker_bps, args.spot_taker_bps)
    latencies = sorted({0, *(int(x) for x in args.latencies.split(","))})
    venue_days = {"coinbase": coinbase_days(args.coinbase_day), "binance": binance_days(args.binance_day)}
    venue_days = {venue: specs for venue, specs in venue_days.items() if specs}
    args.output.mkdir(parents=True, exist_ok=True)
    for stale in ("latency.png",):
        (args.output / stale).unlink(missing_ok=True)
    pairs, day_stats, markouts, coinbase_grid = sweep(venue_days, fees, latencies, args.work, args.jobs)
    results, days = tables(pairs, day_stats, fees, latencies)
    results.to_csv(args.output / "sweep.csv", index=False)
    days.to_csv(args.output / "days.csv", index=False)
    markouts.to_csv(args.output / "markouts.csv", index=False)
    (args.output / "summary.md").write_text(summary_md(results, days, fees, latencies))
    if not args.no_charts:
        charts(results, days, markouts, coinbase_grid, fees, latencies, args.output)
    print((args.output / "summary.md").read_text())


if __name__ == "__main__":
    main()
