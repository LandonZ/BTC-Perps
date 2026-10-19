#!/usr/bin/env python3
"""Reconstruct hourly BIP funding from minute candles, for the historical backtests.

Coinbase does not publish BIP funding history ("available upon request"), so this
applies the published rule (see model/bip-funding.h) to minute candles:
  3-minute mark = volume-weighted typical price (H+L+C)/3 of the bucket's minutes,
                  else the mean close, else the other leg's mark plus the previous gap
  Premium       = mean over the hour's 20 samples of (futures - spot) / spot / 24
  Rate          = 0.75 * Premium + 0.25 * Previous rate
Candles are coarser than the real 3-minute trade VWAP / mid TWAP, so treat the output
as an estimate. "Spot" is BTC-USD here; Coinbase may use its index instead.

Output matches data/coinbase/BTC-PERP-INTX_funding_1h.csv (ts,funding_time,funding_rate,
mark_price), with each rate stamped at the end of the hour it was computed over, so
basis-backtester and paper-trader --candles can load it.
"""
import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    rows = {}
    with open(path, newline="") as file:
        for row in csv.DictReader(file):
            rows[int(row["ts"])] = tuple(float(row[k]) for k in ("high", "low", "close", "volume"))
    if not rows:
        raise SystemExit(f"No candles in {path}")
    return rows


def bucket_mark(candles, start):
    minutes = [candles[t] for t in range(start, start + 180, 60) if t in candles]
    if not minutes:
        return None
    volume = sum(c[3] for c in minutes)
    if volume > 0:
        return sum((c[0] + c[1] + c[2]) / 3 * c[3] for c in minutes) / volume
    return sum(c[2] for c in minutes) / len(minutes)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--perp-csv", default=str(ROOT / "data/coinbase/BIP-20DEC30-CDE_1m.csv"))
    parser.add_argument("--spot-csv", default=str(ROOT / "data/coinbase/BTC-USD_spot_1m.csv"))
    parser.add_argument("--output", default=str(ROOT / "data/coinbase/BIP-20DEC30-CDE_funding_1h_reconstructed.csv"))
    parser.add_argument("--prev-rate", type=float, default=0.0, help="seed rate as a fraction (0.000011 = 0.11 bp)")
    args = parser.parse_args()
    perp, spot = load(args.perp_csv), load(args.spot_csv)
    first = max(min(perp), min(spot))
    last = min(max(perp), max(spot))
    hour = -(-first // 3600) * 3600  # First whole hour; a partial hour has an incomplete TWAP.
    previous_rate, previous_marks, rows, carried = args.prev_rate, None, [], 0
    while hour + 3600 <= last + 60:
        samples = []
        for start in range(hour, hour + 3600, 180):
            f, s = bucket_mark(perp, start), bucket_mark(spot, start)
            if f is None and s is None:
                if previous_marks is None:
                    continue
                f, s = previous_marks
            elif f is None:
                if previous_marks is None:
                    continue
                f = s + (previous_marks[0] - previous_marks[1])
            elif s is None:
                if previous_marks is None:
                    continue
                s = f - (previous_marks[0] - previous_marks[1])
            previous_marks = (f, s)
            samples.append((f - s) / s / 24)
        if samples:
            previous_rate = 0.75 * sum(samples) / len(samples) + 0.25 * previous_rate
        else:
            carried += 1  # No data at all: keep the series gap-free with the last rate.
        end = hour + 3600
        mark = next((perp[t][2] for t in range(end - 60, hour - 60, -60) if t in perp), None)
        if mark is None:
            mark = previous_marks[0] if previous_marks else None
        if mark is not None:
            rows.append([end, datetime.fromtimestamp(end, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                         f"{previous_rate:.10f}", f"{mark:.2f}"])
        hour = end
    if not rows:
        raise SystemExit("No overlapping hours between the two candle files")
    with open(args.output, "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["ts", "funding_time", "funding_rate", "mark_price"])
        writer.writerows(rows)
    rates = [float(r[2]) * 1e4 for r in rows]
    print(f"wrote {len(rows):,} hourly rates to {args.output} ({rows[0][1]} .. {rows[-1][1]}); "
          f"mean {sum(rates) / len(rates):+.3f} bp/h, min {min(rates):+.3f}, max {max(rates):+.3f}; "
          f"{carried} empty hours carried")


if __name__ == "__main__":
    main()
