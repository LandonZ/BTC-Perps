#!/usr/bin/env python3
"""Build Binance BTCUSDT basis-maker replays from Tardis.dev tick data.

Binance's public archives no longer publish recent best bid/ask files, and binance.com
refuses US connections. Tardis.dev redistributes recorded Binance feeds; the FIRST DAY
OF EVERY MONTH is free without an API key (other days need TARDIS_API_KEY / --api-key).

For each --date this downloads, from https://datasets.tardis.dev/v1/:
  binance-futures/book_ticker   perp best bid/ask        -> Q (on bid/ask price change)
  binance-futures/trades        perp trades              -> T (buyer_maker = taker sold)
  binance-futures/derivative_ticker  mark, funding       -> F at each settlement, R seed
  binance/book_ticker           spot best bid/ask        -> S (actual spot spread)
and Binance's own 1m premium-index klines (data.binance.vision) -> P,
then writes data/binance/tardis/YYYY-MM-DD/:
  events.csv          time_ms,kind,price,quantity,bid,ask,buyer_maker,funding_rate,interval_hours
                      strictly chronological, for build/basis-maker --events
  premium-events.csv  P = completed 1m premium-index CLOSE at close_time + 1 ms
                      (build/basis-maker --premium-events; omit with --no-premium)
  manifest.json       source URLs, SHA-256, sizes, row counts and timing repairs
Raw downloads are cached in data/binance/tardis/YYYY-MM-DD/raw/ and reused.

Timing: perp rows use Binance's exchange timestamp. Binance spot bookTicker has no exchange
time, so Tardis stamps S with its own receive time. Exchange timestamps can be slightly out
of order inside a file; rows are re-sorted within --reorder-ms and any row still behind is
written at the previous row's time (counted as "late" in the manifest).

Funding: F is written when derivative_ticker rolls to the next funding time, with the last
rate and mark before settlement. A day file starts with R (the previous settlement time and
the first observed rate, an estimate) unless the first rows still show the 00:00 settlement,
in which case that settlement is written as F with its actual rate.

  python3 Primary-Market-Model/tools/download_tardis_binance.py --date 2026-10-01
  python3 Primary-Market-Model/tools/download_tardis_binance.py --date 2026-08-01 --date 2026-09-01
"""
import argparse
import csv
import gzip
import hashlib
import heapq
import io
import json
import os
import sys
import urllib.error
import urllib.request
import zipfile
from datetime import datetime, timezone
from itertools import count
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARDIS = "https://datasets.tardis.dev/v1/{exchange}/{kind}/{y}/{m}/{d}/{symbol}.csv.gz"
PREMIUM = "https://data.binance.vision/data/futures/um/daily/premiumIndexKlines/{s}/1m/{s}-1m-{day}.zip"
COLUMNS = ["time_ms", "kind", "price", "quantity", "bid", "ask", "buyer_maker", "funding_rate", "interval_hours"]
PRIORITY = {"R": 0, "F": 0, "S": 1, "Q": 2, "T": 3}  # Same tie order as Allen's archive sort.
SOURCES = [("perp_quotes", "binance-futures", "book_ticker"), ("perp_trades", "binance-futures", "trades"),
           ("perp_ticker", "binance-futures", "derivative_ticker"), ("spot_quotes", "binance", "book_ticker")]


def row(time_ms, kind, price="", quantity="", bid="", ask="", buyer="", rate="", interval=""):
    return [time_ms, kind, price, quantity, bid, ask, buyer, rate, interval]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, path, api_key=None):
    """Download once to `path` (atomic rename); reuse a completed file."""
    if not (path.exists() and path.stat().st_size > 0):
        path.parent.mkdir(parents=True, exist_ok=True)
        headers = {"User-Agent": "btc-perps-research"}
        if api_key and "tardis.dev" in url:
            headers["Authorization"] = f"Bearer {api_key}"
        temporary = path.with_name(path.name + ".tmp")
        print(f"Downloading {url}", flush=True)
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as response, \
                    temporary.open("wb") as output:
                received = 0
                while block := response.read(1 << 20):
                    output.write(block)
                    received += len(block)
                    if received % (100 << 20) < (1 << 20):
                        print(f"  {path.name}: {received / 1e6:,.0f} MB", flush=True)
            os.replace(temporary, path)
        except urllib.error.HTTPError as error:
            hint = " (non-free day: set TARDIS_API_KEY or use the 1st of a month)" if error.code in (401, 403) else ""
            raise SystemExit(f"{url}: HTTP {error.code}{hint}") from error
        finally:
            temporary.unlink(missing_ok=True)
    return path, {"url": url, "file": path.name, "bytes": path.stat().st_size, "sha256": sha256(path)}


def tardis_rows(path):
    with gzip.open(path, "rt", newline="") as handle:
        yield from csv.DictReader(handle)


def millis(microseconds):
    return int(microseconds) // 1000


def reorder(rows, window_ms, stats):
    """Sort a nearly-sorted stream within `window_ms`; clamp anything later than that."""
    heap, sequence, last = [], count(), None
    for r in rows:
        heapq.heappush(heap, (r[0], PRIORITY[r[1]], next(sequence), r))
        while heap and heap[0][0] <= r[0] - window_ms:
            item = heapq.heappop(heap)[3]
            if last is not None and item[0] < last:
                item[0] = last
                stats["late"] += 1
            last = item[0]
            yield item
    while heap:
        item = heapq.heappop(heap)[3]
        if last is not None and item[0] < last:
            item[0] = last
            stats["late"] += 1
        last = item[0]
        yield item


def quotes(path, kind):
    """Q (perp) or S (spot) rows, only when the best bid or ask price changes."""
    previous = None
    for r in tardis_rows(path):
        bid, ask = r["bid_price"], r["ask_price"]
        if not bid or not ask or float(bid) <= 0 or float(ask) < float(bid) or (bid, ask) == previous:
            continue
        previous = (bid, ask)
        yield row(millis(r["timestamp"]), kind, bid=bid, ask=ask)


def trades(path):
    for r in tardis_rows(path):
        # Tardis `side` is the taker side: a taker sell means the buyer was the maker.
        yield row(millis(r["timestamp"]), "T", r["price"], r["amount"], buyer="true" if r["side"] == "sell" else "false")


def funding(path, default_hours, stats):
    """R seed (or F if the day opens on a known settlement), then F at each roll-forward."""
    current, written = None, None  # current = (next_funding_ms, rate, mark)
    for r in tardis_rows(path):
        if not r.get("funding_timestamp") or not r.get("funding_rate"):
            continue
        t, next_funding = millis(r["timestamp"]), millis(r["funding_timestamp"])
        rate, mark = r["funding_rate"], r.get("mark_price") or ""
        if current is None:
            if next_funding <= t:  # Settlement passed, not yet rolled: its rate is known.
                yield row(next_funding, "F", mark, rate=rate, interval=f"{default_hours:g}")
                written, stats["day_start"] = next_funding, "F"
            else:
                yield row(next_funding - int(default_hours * 3600000), "R", mark, rate=rate,
                          interval=f"{default_hours:g}")
                stats["day_start"] = "R (rate estimated from the first observation)"
        elif next_funding > current[0] and current[0] != written:
            settled, settled_rate, settled_mark = current
            hours = (next_funding - settled) / 3600000
            yield row(settled, "F", settled_mark, rate=settled_rate, interval=f"{hours:g}")
            written = settled
            stats["settlements"] += 1
        current = (next_funding, rate, mark)


def premium_rows(path):
    with zipfile.ZipFile(path) as archive:
        name = next(n for n in archive.namelist() if n.endswith(".csv"))
        with archive.open(name) as handle:
            for r in csv.reader(io.TextIOWrapper(handle)):
                if r and r[0].isdigit():
                    close = int(r[6])
                    close = close // 1000 if close > 10**14 else close  # Newer archives use microseconds.
                    yield row(close + 1, "P", r[4])


def build_day(day, symbol, api_key, reorder_ms, with_premium, default_hours):
    directory = ROOT / "data/binance/tardis" / day
    raw = directory / "raw"
    y, m, d = day.split("-")
    if d != "01" and not api_key:
        raise SystemExit(f"{day}: only the first day of each month is free; set TARDIS_API_KEY for other days")
    sources, files = {}, {}
    for name, exchange, kind in SOURCES:
        url = TARDIS.format(exchange=exchange, kind=kind, y=y, m=m, d=d, symbol=symbol)
        files[name], sources[name] = download(url, raw / f"{exchange}_{kind}_{symbol}.csv.gz", api_key)
    path = files.get
    stats = {k: {"late": 0} for k in ("perp_quotes", "perp_trades", "spot_quotes", "perp_ticker")}
    stats["perp_ticker"]["settlements"] = 0
    streams = [
        reorder(quotes(path("perp_quotes"), "Q"), reorder_ms, stats["perp_quotes"]),
        reorder(trades(path("perp_trades")), reorder_ms, stats["perp_trades"]),
        reorder(quotes(path("spot_quotes"), "S"), reorder_ms, stats["spot_quotes"]),
        reorder(funding(path("perp_ticker"), default_hours, stats["perp_ticker"]), reorder_ms, stats["perp_ticker"]),
    ]
    counts, first, last = {}, None, None
    output = directory / "events.csv"
    temporary = output.with_name("events.csv.tmp")
    with temporary.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(COLUMNS)
        for r in heapq.merge(*streams, key=lambda r: (r[0], PRIORITY[r[1]])):
            writer.writerow(r)
            counts[r[1]] = counts.get(r[1], 0) + 1
            first = r[0] if first is None else first
            last = r[0]
    missing = [k for k in ("Q", "T", "S") if not counts.get(k)]
    if missing:
        temporary.unlink()
        raise SystemExit(f"{day}: no {', '.join(missing)} rows; check the Tardis files in {raw}")
    os.replace(temporary, output)
    manifest = {"day": day, "symbol": symbol, "sources": sources, "rows": counts,
                "first_utc": utc(first), "last_utc": utc(last), "timing": stats}
    if with_premium:
        archive, source = download(PREMIUM.format(s=symbol, day=day), raw / f"{symbol}-premiumIndex-1m-{day}.zip")
        premium = directory / "premium-events.csv"
        rows = list({r[0]: r for r in premium_rows(archive)}.values())  # One close per minute.
        rows.sort(key=lambda r: r[0])
        with premium.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(COLUMNS)
            writer.writerows(rows)
        manifest["sources"]["premium"] = source
        manifest["rows"]["P"] = len(rows)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2, default=str) + "\n")
    print(f"{day}: {', '.join(f'{k}={v:,}' for k, v in sorted(counts.items()))} "
          f"({utc(first)} .. {utc(last)}) -> {output}", flush=True)


def utc(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z" if ms else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_day = datetime.now(timezone.utc).strftime("%Y-%m-01")
    parser.add_argument("--date", action="append", help=f"UTC day YYYY-MM-DD, repeatable (default {default_day})")
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--api-key", default=os.environ.get("TARDIS_API_KEY"))
    parser.add_argument("--reorder-ms", type=int, default=5000)
    parser.add_argument("--funding-hours", type=float, default=8, help="interval for the day-start R seed")
    parser.add_argument("--no-premium", action="store_true", help="skip data.binance.vision premium klines")
    args = parser.parse_args()
    for day in args.date or [default_day]:
        datetime.strptime(day, "%Y-%m-%d")
        build_day(day, args.symbol.upper(), args.api_key, args.reorder_ms, not args.no_premium, args.funding_hours)


if __name__ == "__main__":
    main()
