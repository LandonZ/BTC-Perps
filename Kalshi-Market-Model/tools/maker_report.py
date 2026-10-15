#!/usr/bin/env python3
"""Compare every maker simulation run under a results folder.

Writes <folder>/report.md (one row per run, with the settings that differ) and
<folder>/pnl.png (cumulative PnL of each run). Reads summary.json and equity.csv.
"""
import argparse
import csv
import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path

SETTINGS = ["edge_bps", "exit_bps", "center", "center_bps", "center_minutes", "holding_minutes", "queue",
            "sides", "quote_latency_s", "hedge_latency_s", "perp_maker_fee_bps", "perp_taker_fee_bps", "spot_fee_bps"]


def fmt(v):
    return f"{v:g}" if isinstance(v, float) else str(v)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "btc-perps-mpl-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    runs = sorted(d for d in args.directory.iterdir() if (d / "summary.json").exists())
    assert runs, f"No runs with summary.json under {args.directory}"
    summaries = {d.name: json.loads((d / "summary.json").read_text()) for d in runs}
    varying = [k for k in SETTINGS if len({json.dumps(s[k]) for s in summaries.values()}) > 1]
    fixed = {k: next(iter(summaries.values()))[k] for k in SETTINGS if k not in varying}

    fig, ax = plt.subplots(figsize=(11, 5.5))
    for d in runs:
        equity_file = d / "equity.csv"
        if not equity_file.exists():
            continue
        with equity_file.open(newline="") as f:
            rows = list(csv.DictReader(f))
        ax.plot([datetime.fromtimestamp(int(r["ts"]), timezone.utc) for r in rows],
                [float(r["net_pnl"]) for r in rows], lw=1, label=d.name)
    ax.axhline(0, color="black", lw=0.6)
    ax.set_ylabel("Cumulative PnL (USD, 0.01 BTC per leg)")
    ax.set_title("Kalshi maker simulation")
    ax.legend(fontsize=7, ncol=2)
    ax.grid(alpha=0.3)
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(args.directory / "pnl.png", dpi=130)

    first = next(iter(summaries.values()))
    lines = [
        "# Kalshi maker simulation",
        "",
        f"UTC {first['start_utc']} through {first['end_utc']}, Kalshi `KXBTCPERP` tick trades vs Coinbase "
        "`BTC-USD` 1-second spot. Rerun with `make maker-report`.",
        "",
        "![Cumulative PnL](pnl.png)",
        "",
        "Settings shared by every run: " + ", ".join(f"`{k}={fmt(v)}`" for k, v in fixed.items()) + ".",
        "",
        "| Run | " + " | ".join(varying) + " | Trades | Win % | Captured bps | Hedge slip bps | Fees | Net PnL | Max DD |",
        "|---|" + "---|" * len(varying) + "---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, s in summaries.items():
        win = 100 * s["winning_trades"] / s["trades"] if s["trades"] else 0
        lines.append(f"| {name} | " + " | ".join(fmt(s[k]) for k in varying)
                     + f" | {s['trades']} | {win:.1f} | {s['mean_entry_basis_bps']:+.2f} | {s['mean_hedge_slip_bps']:+.2f}"
                     f" | {s['fee_pnl']:+.2f} | **{s['net_pnl']:+.2f}** | {s['max_drawdown']:.2f} |")
    lines += [
        "",
        "**Captured bps**: how far from Coinbase spot the perp fill was, in our favour (sold rich / bought cheap).",
        "**Hedge slip bps**: how far spot moved against us between the price we quoted off and the spot hedge.",
        "When slip is about as large as captured, fills are adverse selection: we are filled because our quote is stale.",
        "",
        "## Assumptions",
        "",
        "- **Fills**: Kalshi publishes trades, not the order book. A resting order fills when takers trade at its "
        "price (`queue=front`, optimistic) or through it (`queue=back`, conservative). Fills accumulate to the full size.",
        "- **Best bid/ask**: the last seller-initiated and buyer-initiated prints. Quotes are post-only and never cross them.",
        "- **Our orders change nothing**: no other trader reacts to our quotes, and taker flow is the same with us in the book.",
        "- **Latency**: quotes use spot known `quote_latency_s` before each print; the spot hedge executes at the spot "
        "price `hedge_latency_s` after the fill. Spot is 1-second candles, so delays are rounded to whole seconds.",
        "- **Spot**: unlimited size at the last trade price; no spot spread beyond `spot_fee_bps`.",
        "- **Exits**: a post-only order at fair +/- `exit_bps`; at the holding deadline, cross the Kalshi spread "
        "and pay the taker fee. Funding is charged on positions held through an 8-hour settlement.",
        "- **One position at a time**, `quantity_btc` per leg. No margin, borrow, or liquidation.",
        "",
    ]
    (args.directory / "report.md").write_text("\n".join(lines))
    print(f"Wrote {args.directory / 'report.md'} and pnl.png ({len(runs)} runs)")


if __name__ == "__main__":
    main()
