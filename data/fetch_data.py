#!/usr/bin/env python3
"""Download BTC spot / perp data for the basis + market-making backtests.

Stdlib only (no pandas/numpy needed). Every output is a plain CSV with a header
row and a unix-seconds `ts` first column so the C++ models can parse it easily.
See data/README.md for sources and column definitions.

Usage:
    python3 data/fetch_data.py all                  # everything, default ranges
    python3 data/fetch_data.py kalshi --trades-start 2026-10-01
    python3 data/fetch_data.py coinbase --start 2026-09-01
    python3 data/fetch_data.py binance --start 2026-01-01
    python3 data/fetch_data.py record --seconds 3600   # live Kalshi book + premium recorder
"""
import argparse
import csv
import io
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timedelta, timezone

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
KALSHI = "https://external-api.kalshi.com/trade-api/v2/margin"
CB_ADV = "https://api.coinbase.com/api/v3/brokerage/market/products"
CB_INTX = "https://api.international.coinbase.com/api/v1/instruments"
BINANCE = "https://data.binance.vision/data"

KALSHI_TICKER = "KXBTCPERP"
KALSHI_CONTRACT_SIZE = 0.0001  # BTC per contract; prices are quoted per contract
KALSHI_LAUNCH = "2026-06-03"
INT64_SENTINEL = "922337203685477.5807"  # Kalshi fills empty candle fields with this


# ---------------------------------------------------------------- helpers

def log(msg):
    print(msg, file=sys.stderr, flush=True)


def parse_day(s):
    return datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc)


def iso_to_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()


def http_get(url, params=None, raw=False, retries=5):
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": "ucf-btc-perps/1.0"})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
                return body if raw else json.loads(body)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            if e.code in (429, 500, 502, 503, 504) and attempt < retries - 1:
                time.sleep(2 ** attempt)
                continue
            raise RuntimeError(f"GET {url} -> {e.code}: {e.read()[:300]!r}")
        except (urllib.error.URLError, TimeoutError):
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)


def write_csv(path, header, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    os.replace(tmp, path)
    log(f"  wrote {len(rows):>9,} rows -> {os.path.relpath(path, DATA_DIR)}")


def merge_csv(path, header, rows, key=0):
    """Merge new rows into an existing CSV (dedupe on column `key`, sort by it)."""
    merged = {}
    if os.path.exists(path):
        with open(path, newline="") as f:
            r = csv.reader(f)
            next(r, None)
            for row in r:
                merged[row[key]] = row
    for row in rows:
        merged[str(row[key])] = [str(x) for x in row]
    write_csv(path, header, sorted(merged.values(), key=lambda r: float(r[key])))


def clean(v):
    return "" if v in (None, INT64_SENTINEL) else v


# ---------------------------------------------------------------- Kalshi

def kalshi_funding(start, end):
    log(f"[kalshi] funding rates {KALSHI_TICKER}")
    rows, cursor_end = {}, int(end.timestamp())
    s = int(start.timestamp())
    while True:  # endpoint returns newest-first; walk backwards until nothing new
        d = http_get(f"{KALSHI}/funding_rates/historical",
                     {"ticker": KALSHI_TICKER, "start_ts": s, "end_ts": cursor_end})
        batch = d.get("funding_rates") or []
        new = [x for x in batch if x["funding_time"] not in rows]
        for x in new:
            rows[x["funding_time"]] = x
        if not new:
            break
        oldest = min(iso_to_ts(x["funding_time"]) for x in batch)
        if oldest <= s:
            break
        cursor_end = int(oldest) - 1
    out = [[int(iso_to_ts(t)), t, x["funding_rate"], x["mark_price"],
            round(float(x["mark_price"]) / KALSHI_CONTRACT_SIZE, 2)]
           for t, x in rows.items()]
    out.sort()
    write_csv(os.path.join(DATA_DIR, "kalshi", f"{KALSHI_TICKER}_funding.csv"),
              ["ts", "funding_time", "funding_rate", "mark_price", "mark_price_usd_per_btc"], out)


def kalshi_candles(start, end, interval=1):
    log(f"[kalshi] {interval}m candles {KALSHI_TICKER}")
    step = 4800 * 60 * interval  # API max is 5000 candles per request
    rows, t = [], int(start.timestamp())
    stop = int(end.timestamp())
    while t < stop:
        d = http_get(f"{KALSHI}/markets/{KALSHI_TICKER}/candlesticks",
                     {"start_ts": t, "end_ts": min(t + step, stop), "period_interval": interval})
        for c in d.get("candlesticks") or []:
            b, a, p = c["bid"], c["ask"], c["price"]
            bc, ac = clean(b["close"]), clean(a["close"])
            mid = ""
            if bc and ac and float(bc) > 0 and float(ac) > 0:
                mid = round((float(bc) + float(ac)) / 2 / KALSHI_CONTRACT_SIZE, 2)
            rows.append([c["end_period_ts"],
                         clean(b["open"]), clean(b["high"]), clean(b["low"]), bc,
                         clean(a["open"]), clean(a["high"]), clean(a["low"]), ac,
                         clean(p["open"]), clean(p["high"]), clean(p["low"]), clean(p["close"]),
                         clean(p.get("mean")), c["volume"], c["volume_notional_value_dollars"],
                         c["open_interest"], mid])
        t += step
        time.sleep(0.1)
    rows = list({r[0]: r for r in rows}.values())
    rows.sort(key=lambda r: r[0])
    write_csv(os.path.join(DATA_DIR, "kalshi", f"{KALSHI_TICKER}_candles_{interval}m.csv"),
              ["ts", "bid_open", "bid_high", "bid_low", "bid_close",
               "ask_open", "ask_high", "ask_low", "ask_close",
               "trade_open", "trade_high", "trade_low", "trade_close", "trade_mean",
               "volume_contracts", "volume_usd", "open_interest", "mid_close_usd_per_btc"], rows)


def kalshi_trades(start, end):
    """Every public trade. Pulled one day at a time so a crash only loses a day."""
    log(f"[kalshi] trades {KALSHI_TICKER} {start.date()} -> {end.date()}")
    out_dir = os.path.join(DATA_DIR, "kalshi", "trades")
    day = start.replace(hour=0, minute=0, second=0, microsecond=0)
    today = datetime.now(timezone.utc).date()
    while day < end:
        path = os.path.join(out_dir, f"{KALSHI_TICKER}_trades_{day.date()}.csv")
        if os.path.exists(path) and day.date() < today:
            log(f"  skip {day.date()} (already downloaded)")
            day += timedelta(days=1)
            continue
        rows, cursor = [], None
        params = {"ticker": KALSHI_TICKER, "limit": 1000,
                  "min_ts": int(day.timestamp()), "max_ts": int((day + timedelta(days=1)).timestamp())}
        while True:
            if cursor:
                params["cursor"] = cursor
            d = http_get(f"{KALSHI}/trades", params)
            for x in d.get("trades") or []:
                rows.append([f"{iso_to_ts(x['created_time']):.6f}", x["price"], x["count"],
                             x["taker_side"], x["trade_id"]])
            cursor = d.get("cursor")
            if not cursor or not d.get("trades"):
                break
            time.sleep(0.05)
        rows.sort(key=lambda r: float(r[0]))
        write_csv(path, ["ts", "price", "count", "taker_side", "trade_id"], rows)
        day += timedelta(days=1)


def kalshi_premium_index():
    """Per-second premium index. Kalshi only keeps ~7 days, so merge into the
    existing file each run to build a longer history over time."""
    log(f"[kalshi] per-second premium index {KALSHI_TICKER} (last ~7 days)")
    now = int(time.time())
    rows, empty_streak = [], 0
    t = now - 3600
    while empty_streak < 6:  # stop after 6 consecutive empty hours (past retention)
        d = http_get(f"{KALSHI}/funding_rates/premium_index",
                     {"ticker": KALSHI_TICKER, "start_ts": t, "end_ts": t + 3599})
        pts = (d or {}).get("points") or []
        empty_streak = 0 if pts else empty_streak + 1
        rows += [[int(iso_to_ts(p["second_ts"])), p["premium_index"]] for p in pts]
        t -= 3600
        time.sleep(0.05)
    merge_csv(os.path.join(DATA_DIR, "kalshi", f"{KALSHI_TICKER}_premium_index_1s.csv"),
              ["ts", "premium_index"], rows)


def kalshi_market_snapshot():
    d = http_get(f"{KALSHI}/markets/{KALSHI_TICKER}")
    os.makedirs(os.path.join(DATA_DIR, "kalshi"), exist_ok=True)
    with open(os.path.join(DATA_DIR, "kalshi", f"{KALSHI_TICKER}_market_info.json"), "w") as f:
        json.dump(d, f, indent=2)


def record(seconds, interval):
    """Poll top-of-book (10 levels) + funding estimate. Run this in the background
    to collect depth data that Kalshi does not serve historically."""
    path = os.path.join(DATA_DIR, "kalshi", "live",
                        f"{KALSHI_TICKER}_book_{datetime.now(timezone.utc):%Y%m%dT%H%M%S}.csv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    levels = 10
    header = ["ts"] + [f"bid_px_{i}" for i in range(levels)] + [f"bid_sz_{i}" for i in range(levels)] \
        + [f"ask_px_{i}" for i in range(levels)] + [f"ask_sz_{i}" for i in range(levels)] \
        + ["premium_index", "est_funding_rate", "mark_price"]
    log(f"[record] -> {os.path.relpath(path, DATA_DIR)} every {interval}s for {seconds}s (Ctrl-C to stop)")
    end = time.time() + seconds
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        try:
            while time.time() < end:
                t0 = time.time()
                ob = http_get(f"{KALSHI}/markets/{KALSHI_TICKER}/orderbook", {"depth": levels})["orderbook"]
                est = http_get(f"{KALSHI}/funding_rates/estimate", {"ticker": KALSHI_TICKER})
                bids = sorted(ob.get("bids") or [], key=lambda x: -float(x[0]))[:levels]
                asks = sorted(ob.get("asks") or [], key=lambda x: float(x[0]))[:levels]
                pad = lambda lv, k: [x[k] for x in lv] + [""] * (levels - len(lv))
                w.writerow([f"{t0:.3f}"] + pad(bids, 0) + pad(bids, 1) + pad(asks, 0) + pad(asks, 1)
                           + [est.get("premium_index"), est.get("funding_rate"), est.get("mark_price")])
                f.flush()
                time.sleep(max(0.0, interval - (time.time() - t0)))
        except KeyboardInterrupt:
            log("[record] stopped")


# ---------------------------------------------------------------- Coinbase

def coinbase_candles(product, start, end, name):
    """1m candles from the public Advanced Trade endpoint (max 350 per request)."""
    log(f"[coinbase] 1m candles {product}")
    step = 350 * 60
    rows, t = {}, int(start.timestamp())
    stop = int(end.timestamp())
    while t < stop:
        d = http_get(f"{CB_ADV}/{product}/candles",
                     {"granularity": "ONE_MINUTE", "start": t, "end": min(t + step - 60, stop)})
        for c in (d or {}).get("candles") or []:
            rows[int(c["start"])] = [int(c["start"]), c["open"], c["high"], c["low"], c["close"], c["volume"]]
        t += step
        time.sleep(0.11)  # public limit is 10 req/s
    write_csv(os.path.join(DATA_DIR, "coinbase", f"{name}_1m.csv"),
              ["ts", "open", "high", "low", "close", "volume"], sorted(rows.values()))


def coinbase_intx_funding(start):
    log("[coinbase] INTX BTC-PERP hourly funding")
    rows, offset = [], 0
    s = start.timestamp()
    while True:
        d = http_get(f"{CB_INTX}/BTC-PERP/funding", {"result_limit": 500, "result_offset": offset})
        res = d.get("results") or []
        rows += [[int(iso_to_ts(x["event_time"])), x["event_time"], x["funding_rate"], x["mark_price"]]
                 for x in res]
        if not res or iso_to_ts(res[-1]["event_time"]) < s:
            break
        offset += len(res)
        time.sleep(0.1)
    rows = sorted({r[0]: r for r in rows if r[0] >= s}.values())
    write_csv(os.path.join(DATA_DIR, "coinbase", "BTC-PERP-INTX_funding_1h.csv"),
              ["ts", "funding_time", "funding_rate", "mark_price"], rows)


# ---------------------------------------------------------------- Binance (bulk archive)

BINANCE_SETS = {
    "spot_klines_1m": "spot/{freq}/klines/BTCUSDT/1m/BTCUSDT-1m-{date}.zip",
    "perp_klines_1m": "futures/um/{freq}/klines/BTCUSDT/1m/BTCUSDT-1m-{date}.zip",
    "perp_premium_index_1m": "futures/um/{freq}/premiumIndexKlines/BTCUSDT/1m/BTCUSDT-1m-{date}.zip",
    "perp_mark_price_1m": "futures/um/{freq}/markPriceKlines/BTCUSDT/1m/BTCUSDT-1m-{date}.zip",
}
KLINE_HEADER = ["ts", "open", "high", "low", "close", "volume", "quote_volume", "num_trades",
                "taker_buy_base_volume", "taker_buy_quote_volume"]


def _binance_zip_rows(url):
    body = http_get(url, raw=True)
    if body is None:
        return None
    with zipfile.ZipFile(io.BytesIO(body)) as z:
        text = z.read(z.namelist()[0]).decode()
    rows = list(csv.reader(io.StringIO(text)))
    if rows and not rows[0][0].lstrip("-").isdigit():
        rows = rows[1:]  # drop header
    return rows


def _binance_periods(start, end):
    """Whole months via monthly archives, the current partial month via daily ones."""
    out, m = [], start.replace(day=1)
    this_month = datetime.now(timezone.utc).replace(day=1, hour=0, minute=0, second=0, microsecond=0)
    while m < end:
        if m < this_month:
            out.append(("monthly", m.strftime("%Y-%m")))
        else:
            d = max(m, start)
            while d < end and d.date() < datetime.now(timezone.utc).date():
                out.append(("daily", d.strftime("%Y-%m-%d")))
                d += timedelta(days=1)
        m = (m + timedelta(days=32)).replace(day=1)
    return out


def binance(start, end):
    periods = _binance_periods(start, end)
    for name, tmpl in BINANCE_SETS.items():
        log(f"[binance] {name} ({len(periods)} files)")
        rows = []
        for freq, date in periods:
            got = _binance_zip_rows(f"{BINANCE}/{tmpl.format(freq=freq, date=date)}")
            if got is None:
                log(f"  missing {date}")
                continue
            for r in got:
                ot = int(r[0])
                ts = ot // 1_000_000 if ot > 10**14 else ot // 1000  # spot switched to microseconds in 2025
                rows.append([ts, r[1], r[2], r[3], r[4], r[5], r[7], r[8], r[9], r[10]])
        rows = sorted({r[0]: r for r in rows if start.timestamp() <= r[0] < end.timestamp()}.values())
        write_csv(os.path.join(DATA_DIR, "binance", f"BTCUSDT_{name}.csv"), KLINE_HEADER, rows)

    log("[binance] perp funding rate (8h)")
    rows = []
    for m in sorted({d[:7] for _, d in periods}):
        got = _binance_zip_rows(f"{BINANCE}/futures/um/monthly/fundingRate/BTCUSDT/BTCUSDT-fundingRate-{m}.zip")
        rows += [[int(r[0]) // 1000, r[1], r[2]] for r in (got or [])]
    rows = sorted({r[0]: r for r in rows if r[0] >= start.timestamp()}.values())
    write_csv(os.path.join(DATA_DIR, "binance", "BTCUSDT_perp_funding.csv"),
              ["ts", "funding_interval_hours", "funding_rate"], rows)


# ---------------------------------------------------------------- CLI

def main():
    now = datetime.now(timezone.utc)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", choices=["all", "kalshi", "coinbase", "binance", "record"])
    ap.add_argument("--start", default=KALSHI_LAUNCH, help="YYYY-MM-DD (default: Kalshi BTC perp launch)")
    ap.add_argument("--end", default=None, help="YYYY-MM-DD exclusive (default: now)")
    ap.add_argument("--trades-start", default=None,
                    help="first day of Kalshi tick trades (default: 7 days ago; tick data is large)")
    ap.add_argument("--seconds", type=int, default=3600, help="record: how long to run")
    ap.add_argument("--interval", type=float, default=1.0, help="record: seconds between snapshots")
    a = ap.parse_args()

    start = parse_day(a.start)
    end = parse_day(a.end) if a.end else now
    trades_start = parse_day(a.trades_start) if a.trades_start else (now - timedelta(days=7)).replace(
        hour=0, minute=0, second=0, microsecond=0)

    if a.source == "record":
        return record(a.seconds, a.interval)
    if a.source in ("all", "kalshi"):
        kalshi_market_snapshot()
        kalshi_funding(start, end)
        kalshi_candles(start, end, 1)
        kalshi_premium_index()
        kalshi_trades(max(trades_start, start), end)
    if a.source in ("all", "coinbase"):
        coinbase_candles("BTC-USD", start, end, "BTC-USD_spot")
        coinbase_candles("BIP-20DEC30-CDE", start, end, "BIP-20DEC30-CDE_us_perp")
        coinbase_candles("BTC-PERP-INTX", start, min(end, parse_day("2026-10-02")), "BTC-PERP-INTX")
        coinbase_intx_funding(start)
    if a.source in ("all", "binance"):
        binance(start, end)


if __name__ == "__main__":
    main()
