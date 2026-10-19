#!/usr/bin/env python3
"""Convert a Coinbase recording into basis-maker replay events.

Reads data/live/YYYY-MM-DD/events.csv[.gz] and funding.csv[.gz] from
tools/record_coinbase.py and writes data/live/YYYY-MM-DD/basis-events.csv.gz in
the format build/basis-maker reads (time_ms,kind,price,quantity,bid,ask,
buyer_maker,funding_rate,interval_hours):

  BIP top of book      -> Q  (perp bid/ask, on change)
  BIP trades           -> T  (quantity in BTC = contracts * 0.01)
  BTC-USD top of book  -> S  (real spot bid/ask, on change)
  hourly settlements   -> F  at each UTC hour inside the recording, with the
                            published rate for that hour and the last BIP mid
  first event          -> R  seed at the preceding hour (interval 1 h)

The clock is recv_ms (this laptop's receive time) for both products, so the two
legs are on one clock. Coinbase's trade `side` is the MAKER's side (BUY prints
mostly hit the bid), so buyer_maker = side == BUY.

Funding: the product API's funding_rate/funding_time is the last settled hour,
published within seconds of the hour. An hour whose rate was never polled (the
recorder was off) gets the previous hour's rate and is counted as estimated.

  python3 Primary-Market-Model/tools/coinbase_basis_events.py --date 2026-10-19
"""
import argparse
import calendar
import csv
import gzip
import io
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PERP, SPOT = "BIP-20DEC30-CDE", "BTC-USD"
CONTRACT_BTC = 0.01
HOUR_MS = 3_600_000
COLUMNS = ["time_ms", "kind", "price", "quantity", "bid", "ask", "buyer_maker", "funding_rate", "interval_hours"]


def open_text(path):
    gz = path.with_name(path.name + ".gz")
    if path.exists():
        return open(path, newline="")
    if gz.exists():
        return io.TextIOWrapper(gzip.open(gz), newline="")
    raise SystemExit(f"Missing {path} (or {gz.name})")


def hour_ms(text):
    return calendar.timegm(time.strptime(text.rstrip("Z"), "%Y-%m-%dT%H:%M:%S")) * 1000


def published_rates(directory):
    """funding_time (ms) -> rate as a fraction, from the first poll that shows it."""
    rates = {}
    with open_text(directory / "funding.csv") as handle:
        for row in csv.DictReader(handle):
            if row["product"] == PERP and row["funding_rate"] and row["funding_time"]:
                rates.setdefault(hour_ms(row["funding_time"]), float(row["funding_rate"]))
    return rates


def convert(directory, output):
    rates = published_rates(directory)
    counts = {k: 0 for k in "QTSFR"}
    estimated, rate, mid = 0, None, None
    last_time, next_hour = None, None
    books = {PERP: None, SPOT: None}
    with open_text(directory / "events.csv") as source, gzip.open(output, "wt", newline="") as sink:
        writer = csv.writer(sink)
        writer.writerow(COLUMNS)
        for row in csv.DictReader(source):
            if row["recv_ms"] == "recv_ms" or row["product"] not in books:
                continue  # Repeated header from concatenated files, or another product.
            t = int(row["recv_ms"])
            if last_time is not None and t < last_time:
                t = last_time  # The laptop clock stepped back; keep the replay chronological.
            if next_hour is None:
                previous = t // HOUR_MS * HOUR_MS
                rate = rates.get(previous, next(iter(rates.values()), 0.0))
                writer.writerow([previous, "R", "", "", "", "", "", f"{rate:.10f}", 1])
                counts["R"] += 1
                next_hour = previous + HOUR_MS
            while t >= next_hour:
                if next_hour in rates:
                    rate = rates[next_hour]
                else:
                    estimated += 1
                if mid is not None:
                    writer.writerow([next_hour, "F", f"{mid:.4f}", "", "", "", "", f"{rate:.10f}", 1])
                    counts["F"] += 1
                next_hour += HOUR_MS
            last_time = t
            product, kind = row["product"], row["kind"]
            if kind == "Q":
                bid, ask = float(row["bid"]), float(row["ask"])
                if bid <= 0 or ask < bid or books[product] == (bid, ask):
                    continue
                books[product] = (bid, ask)
                if product == PERP:
                    mid = (bid + ask) / 2
                    writer.writerow([t, "Q", "", "", row["bid"], row["ask"], "", "", ""])
                    counts["Q"] += 1
                else:
                    writer.writerow([t, "S", "", "", row["bid"], row["ask"], "", "", ""])
                    counts["S"] += 1
            elif kind == "T" and product == PERP:
                quantity = float(row["size"]) * CONTRACT_BTC
                buyer_maker = "true" if row["side"].upper() == "BUY" else "false"
                writer.writerow([t, "T", row["price"], f"{quantity:.4f}", "", "", buyer_maker, "", ""])
                counts["T"] += 1
    return counts, estimated


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", action="append", required=True, help="recording day YYYY-MM-DD, repeatable")
    parser.add_argument("--root", type=Path, default=ROOT / "data/live")
    args = parser.parse_args()
    for day in args.date:
        directory = args.root / day
        output = directory / "basis-events.csv.gz"
        counts, estimated = convert(directory, output)
        print(f"{day}: {counts} estimated_funding_hours={estimated} -> {output}", file=sys.stderr)


if __name__ == "__main__":
    main()
