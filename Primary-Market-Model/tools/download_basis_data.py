#!/usr/bin/env python3
"""Fetch public Coinbase spot/perp minute candles (input to reconstruct_bip_funding.py)."""
import argparse
import csv
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = "https://api.coinbase.com/api/v3/brokerage/market/products"


def timestamp(day):
    return int(datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())


def fetch(product, name, start, end):
    path = ROOT / "data/coinbase" / name
    rows = {}
    if path.exists():
        with path.open(newline="") as file:
            for row in csv.DictReader(file):
                ts = int(row["ts"])  # Keep rows outside this range: other runs may need them.
                rows[ts] = [ts, row["open"], row["high"], row["low"], row["close"], row["volume"]]
    print(f"{product}: downloading historical minute candles", flush=True)
    # Legacy INTX truncates larger requests at 300 candles.
    for chunk, begin in enumerate(range(start, end, 300 * 60)):
        stop = min(begin + 300 * 60, end)
        if all(ts in rows for ts in range(begin, stop, 60)):
            continue
        query = urllib.parse.urlencode({"granularity": "ONE_MINUTE", "start": begin, "end": stop - 1})
        for attempt in range(5):
            try:
                with urllib.request.urlopen(f"{BASE}/{product}/candles?{query}", timeout=30) as response:
                    data = json.load(response)
                break
            except (urllib.error.URLError, TimeoutError):
                if attempt == 4:
                    raise
                time.sleep(2 ** attempt)
        for candle in data.get("candles", []):
            ts = int(candle["start"])
            if start <= ts < end:
                rows[ts] = [ts, candle["open"], candle["high"], candle["low"], candle["close"], candle["volume"]]
        if (chunk + 1) % 100 == 0:
            print(f"{product}: {len(rows):,} candles", flush=True)
        time.sleep(0.25)
    if not rows:
        raise RuntimeError(f"No prices returned for {product}; existing file preserved")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".csv.tmp")
    with temporary.open("w", newline="") as file:
        out = csv.writer(file)
        out.writerow(["ts", "open", "high", "low", "close", "volume"])
        out.writerows(rows[ts] for ts in sorted(rows))
    os.replace(temporary, path)
    print(f"{product}: wrote {len(rows):,} rows to {path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", default="2026-06-03")
    parser.add_argument("--end", default="2026-10-01", help="exclusive; legacy INTX history ends at migration")
    parser.add_argument("--perp", default="BTC-PERP-INTX",
                        help="perp product, e.g. BIP-20DEC30-CDE for the Coinbase US nano BTC perp")
    args = parser.parse_args()
    start, end = timestamp(args.start), timestamp(args.end)
    if end <= start:
        parser.error("end must follow start")
    with ThreadPoolExecutor(max_workers=2) as pool:
        jobs = [pool.submit(fetch, product, name, start, end) for product, name in [
            ("BTC-USD", "BTC-USD_spot_1m.csv"), (args.perp, f"{args.perp}_1m.csv")]]
        for job in jobs:
            job.result()


if __name__ == "__main__":
    main()
