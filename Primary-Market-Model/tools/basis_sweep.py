#!/usr/bin/env python3
"""Run the basis strategy on recorded Coinbase and Binance data and chart the results.

For each venue this runs build/basis-maker at several entry offsets (fair basis +/-
offset) with Shreev's exits: close a pair when the basis returns to fair value,
close anything left at the end of the data, and never hold a payer through
funding. Then, from the pair ledger:

  * fee sweep: net bps per round trip for total cycle fees (1 perp maker + 3 taker
    fills, split equally) from 0 to 4 bps. Fees do not change any decision in the
    engine, so this equals rerunning it with --cycle-fee-bps.
  * hedge latency: reprices the spot hedge and both exit legs at the book in force
    L ms later (L = 0, 50, 250). Decisions and fills are unchanged, so this is the
    cost of acting late, not a full latency replay.

It also measures basis markouts (how far the mid basis moves back toward fair value
after a deviation) on a 1-second grid.

Outputs go to results/pitch/: sweep.csv, markouts.csv, summary.md and PNG charts.

  make -C Primary-Market-Model build/basis-maker
  python3 Primary-Market-Model/tools/basis_sweep.py
"""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd

MODEL = Path(__file__).resolve().parents[1]
ROOT = MODEL.parent
OFFSETS = [0.5, 1.0, 1.5, 2.0]
FEES = [0, 0.5, 1, 1.5, 2, 2.5, 3, 4]
LATENCIES = [0, 50, 250]
HORIZONS = [1, 5, 10, 30, 60, 300, 900]

VENUES = {
    "coinbase": {
        "label": "Coinbase BIP vs BTC-USD (2026-10-19, 10.7 h)",
        "events": ROOT / "data/live/2026-10-19/basis-events.csv.gz",
        # Fair basis is a 10-minute EMA: BIP has no -5 bp funding kink and trades ~3 bp rich.
        # BIP is 1-contract (0.01 BTC) on a $5 tick; trade sizes are whole contracts, so
        # participation is 100% of a print, but only prints THROUGH our price fill us.
        "args": ["--center-halflife-seconds", "600", "--exit-before-funding", "0", "--funding-cancel-seconds", "0",
                 "--tick", "5", "--quantity-step", "0.01", "--participation", "1", "--fill-at-touch", "0"],
    },
    "binance": {
        "label": "Binance BTCUSDT perp vs spot (2026-10-01, 24 h)",
        "events": ROOT / "data/binance/tardis/2026-10-01/events.csv.gz",
        "premium": ROOT / "data/binance/tardis/2026-10-01/premium-events.csv.gz",
        # Fixed -5 bp equilibrium from Binance's funding formula; Allen's queue approximation.
        "args": ["--equilibrium-bps", "-5", "--participation", "0.1", "--fill-at-touch", "1"],
    },
}
COMMON = ["--exit-on-equilibrium", "1", "--close-at-end", "1", "--order-btc", "0.01",
          "--max-gross-perp-btc", "0.1", "--cycle-fee-bps", "0", "--details", "1"]

# Reference palette (dataviz skill): surface, ink and the first categorical slots.
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#6250d6"]


def run_engine(venue, offset, out):
    spec = VENUES[venue]
    cmd = [str(MODEL / "build/basis-maker"), "--events", str(spec["events"]), "--output", str(out),
           "--entry-offset-bps", str(offset), *COMMON, *spec["args"]]
    if "premium" in spec:
        cmd += ["--premium-events", str(spec["premium"])]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return json.loads((out / "summary.json").read_text())


def books(venue):
    """Perp (Q) and spot (S) top of book as time-indexed arrays."""
    e = pd.read_csv(VENUES[venue]["events"], usecols=["time_ms", "kind", "bid", "ask"])
    out = {}
    for kind, leg in (("Q", "perp"), ("S", "spot")):
        x = e[e.kind == kind].drop_duplicates("time_ms", keep="last")
        out[leg] = (x.time_ms.to_numpy(), x.bid.to_numpy(), x.ask.to_numpy())
    return out


def price_at(book, times, side_bid):
    t, bid, ask = book
    i = np.searchsorted(t, times, side="right") - 1
    i = np.clip(i, 0, len(t) - 1)
    return np.where(side_bid, bid[i], ask[i])


def latency_cost_bps(pairs, book, latency):
    """Average change in pair PnL (bps of spot notional) when hedge and exit legs act `latency` ms late."""
    if latency == 0 or pairs.empty:
        return 0.0
    d = np.where(pairs.direction == "LONG_PERP_SHORT_SPOT", 1, -1)
    t0, t1 = pairs.time_ms.to_numpy(), pairs.exit_time_ms.to_numpy()
    long_ = d > 0
    entry_spot = price_at(book["spot"], t0 + latency, long_)        # long perp sells spot at the bid
    exit_perp = price_at(book["perp"], t1 + latency, long_)         # long perp exits at the perp bid
    exit_spot = price_at(book["spot"], t1 + latency, ~long_)        # and buys spot back at the ask
    change = d * ((entry_spot - pairs.spot_price) + (exit_perp - pairs.exit_perp_price)
                  - (exit_spot - pairs.exit_spot_price))
    return float(np.average(change / pairs.spot_price * 1e4, weights=pairs.perp_btc))


def sweep(work):
    rows = []
    for venue in VENUES:
        book = books(venue)
        hours = (book["perp"][0][-1] - book["perp"][0][0]) / 3.6e6
        for offset in OFFSETS:
            out = work / f"{venue}-offset-{offset}"
            summary = run_engine(venue, offset, out)
            pairs = pd.read_csv(out / "pairs.csv")
            pairs = pairs[pairs.status == "CLOSED"]
            notional = pairs.perp_btc * pairs.spot_price
            gross_bps = float((pairs.realized_pnl.sum()) / notional.sum() * 1e4)
            for latency in LATENCIES:
                slip = latency_cost_bps(pairs, book, latency)
                for fee in FEES:
                    net_bps = gross_bps + slip - fee
                    rows.append({
                        "venue": venue, "entry_offset_bps": offset, "latency_ms": latency, "cycle_fee_bps": fee,
                        "round_trips": len(pairs), "btc_traded": round(pairs.perp_btc.sum(), 4),
                        "hours": round(hours, 2), "mean_hold_s": round(summary["closed_pair_mean_hold_seconds"], 2),
                        "funding_usd": round(summary["funding_pnl"], 4),
                        "gross_bps": round(gross_bps, 3), "latency_bps": round(slip, 3), "net_bps": round(net_bps, 3),
                        "net_usd": round(net_bps / 1e4 * notional.sum(), 2),
                    })
    return pd.DataFrame(rows)


def basis_grid(venue, step=1000):
    b = books(venue)
    t0 = max(b["perp"][0][0], b["spot"][0][0]) // step * step + step
    t1 = min(b["perp"][0][-1], b["spot"][0][-1])
    t = np.arange(t0, t1, step)
    out = {}
    for leg in ("perp", "spot"):
        times, bid, ask = b[leg]
        i = np.searchsorted(times, t, side="right") - 1
        out[leg] = (bid[i] + ask[i]) / 2
        out[leg + "_age"] = t - times[i]
    fresh = (out["perp_age"] < 10000) & (out["spot_age"] < 10000)
    basis = 1e4 * (out["perp"] / out["spot"] - 1)
    return pd.Series(np.where(fresh, basis, np.nan), index=pd.to_datetime(t, unit="ms", utc=True))


def markouts(venue, basis):
    if venue == "binance":
        center = pd.Series(-5.0, index=basis.index)
    else:
        center = basis.ewm(halflife="600s", times=basis.index).mean().shift(1)
    b, dev = basis.to_numpy(), (basis - center).to_numpy()
    rows = []
    for lo, hi in [(0.5, 1.0), (1.0, 1.5), (1.5, 2.0), (2.0, 99)]:
        idx = np.where((np.abs(dev) >= lo) & (np.abs(dev) < hi))[0]
        for h in HORIZONS:
            j = idx[idx + h < len(b)]
            move = -np.sign(dev[j]) * (b[j + h] - b[j])
            rows.append({"venue": venue, "deviation_bps": f"{lo:g}-{hi:g}" if hi < 99 else f">{lo:g}",
                         "horizon_s": h, "seconds": int(np.isfinite(move).sum()),
                         "reversion_bps": round(float(np.nanmean(move)), 3)})
    return pd.DataFrame(rows)


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


def bottom_legend(fig, ax, title):
    handles, labels = ax.get_legend_handles_labels()
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    fig.legend(handles, labels, loc="lower center", ncol=len(labels), frameon=False, labelcolor=INK2,
               fontsize=9, title=title, title_fontsize=9)


def charts(results, grids, marks, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"figure.facecolor": SURFACE, "font.size": 10, "lines.linewidth": 2})

    # 1. The basis over time, with the fair value the strategy trades around.
    for venue, basis in grids.items():
        fig, ax = plt.subplots(figsize=(9, 3.6))
        minute = basis.resample("1min").mean()
        ax.plot(minute.index, minute.values, color=SERIES[0], linewidth=1.5)
        note = f"mean {basis.mean():+.2f} bp · median {basis.median():+.2f} bp"
        if venue == "binance":
            ax.axhline(-5, color=INK2, linewidth=1, linestyle="--")
            note += "\ndashed: −5 bp funding equilibrium"
        else:
            note += "\nBIP trades rich to spot; fair value drifts"
        ax.text(0.01, 0.04, note, transform=ax.transAxes, color=INK, fontsize=9, va="bottom",
                bbox={"facecolor": SURFACE, "edgecolor": GRID, "boxstyle": "round,pad=0.4"})
        style(ax, f"Perp − spot basis, 1-minute mean · {VENUES[venue]['label']}", "UTC", "basis (bp)")
        fig.autofmt_xdate()
        fig.tight_layout()
        fig.savefig(out / f"basis-{venue}.png", dpi=160)
        plt.close(fig)

    # 2. Markouts: how much of a deviation reverts, and how fast.
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
    for ax, venue in zip(axes, VENUES):
        m = marks[marks.venue == venue]
        for color, (bucket, g) in zip(SERIES, m.groupby("deviation_bps", sort=False)):
            ax.plot(g.horizon_s, g.reversion_bps, color=color, marker="o", markersize=4, label=f"{bucket} bp off")
        ax.set_xscale("log")
        ax.axhline(0, color=INK2, linewidth=1)
        center = "−5 bp" if venue == "binance" else "10-min EMA"
        style(ax, f"{venue.capitalize()}: reversion toward {center}", "seconds after deviation", "basis move toward fair (bp)")
    bottom_legend(fig, axes[0], "deviation from fair value")
    fig.savefig(out / "markouts.png", dpi=160)
    plt.close(fig)

    # 3. Fees decide it: net bps per round trip vs total cycle fee, zero latency.
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
    for ax, venue in zip(axes, VENUES):
        r = results[(results.venue == venue) & (results.latency_ms == 0)]
        for color, offset in zip(SERIES, OFFSETS):
            g = r[r.entry_offset_bps == offset]
            n = int(g.round_trips.iloc[0])
            ax.plot(g.cycle_fee_bps, g.net_bps, color=color, marker="o", markersize=4,
                    label=f"enter ±{offset:g} bp ({n:,} trips)")
        ax.axhline(0, color=INK2, linewidth=1)
        ax.axvline(2, color=INK2, linewidth=1, linestyle="--")
        ax.annotate("Virtu-tier ~2 bp", (2, ax.get_ylim()[1]), xytext=(4, -12), textcoords="offset points",
                    color=INK2, fontsize=9)
        style(ax, f"{venue.capitalize()}: net per round trip", "total fees per cycle (bp)", "net bp per round trip")
        ax.legend(frameon=False, labelcolor=INK2, fontsize=8, loc="lower left")
    fig.tight_layout()
    fig.savefig(out / "fees.png", dpi=160)
    plt.close(fig)

    # 4. Speed: what acting 50 / 250 ms late costs, at Virtu-tier fees.
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
    for ax, venue in zip(axes, VENUES):
        r = results[(results.venue == venue) & (results.cycle_fee_bps == 2)]
        for color, latency in zip(SERIES, LATENCIES):
            g = r[r.latency_ms == latency]
            ax.plot(g.entry_offset_bps, g.net_bps, color=color, marker="o", markersize=4, label=f"{latency} ms")
        ax.axhline(0, color=INK2, linewidth=1)
        style(ax, f"{venue.capitalize()}: net at 2 bp fees vs hedge delay", "entry offset from fair (bp)", "net bp per round trip")
    bottom_legend(fig, axes[0], "hedge and exit delay")
    fig.savefig(out / "latency.png", dpi=160)
    plt.close(fig)


def summary_md(results, grids, marks):
    lines = ["# Basis strategy on recorded data", ""]
    for venue, basis in grids.items():
        lines.append(f"- **{VENUES[venue]['label']}**: basis mean {basis.mean():+.2f} bp, median {basis.median():+.2f}, "
                     f"std {basis.std():.2f} (1-second grid, {basis.notna().sum():,} fresh seconds).")
    lines += ["", "Net bps per round trip, zero latency (gross = before fees):", "",
              "| venue | offset | trips | BTC | hold s | gross | 1 bp fees | 2 bp fees | 3 bp fees | 2 bp + 50 ms | 2 bp + 250 ms |",
              "|---|---|---|---|---|---|---|---|---|---|---|"]
    for (venue, offset), g in results.groupby(["venue", "entry_offset_bps"], sort=False):
        at = lambda fee, lat: g[(g.cycle_fee_bps == fee) & (g.latency_ms == lat)].net_bps.iloc[0]
        first = g.iloc[0]
        lines.append(f"| {venue} | ±{offset:g} | {first.round_trips:,} | {first.btc_traded:g} | {first.mean_hold_s:g} | "
                     f"{at(0, 0):+.2f} | {at(1, 0):+.2f} | {at(2, 0):+.2f} | {at(3, 0):+.2f} | {at(2, 50):+.2f} | {at(2, 250):+.2f} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=MODEL / "results/pitch")
    parser.add_argument("--work", type=Path, default=MODEL / "build/sweep")
    parser.add_argument("--no-charts", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = sweep(args.work)
    grids = {venue: basis_grid(venue) for venue in VENUES}
    marks = pd.concat([markouts(venue, grids[venue]) for venue in VENUES])
    results.to_csv(args.output / "sweep.csv", index=False)
    marks.to_csv(args.output / "markouts.csv", index=False)
    (args.output / "summary.md").write_text(summary_md(results, grids, marks))
    if not args.no_charts:
        charts(results, grids, marks, args.output)
    print((args.output / "summary.md").read_text())


if __name__ == "__main__":
    main()
