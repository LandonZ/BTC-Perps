#!/usr/bin/env python3
"""Record Coinbase BTC-USD spot and BIP perp-style futures ticks for basis replays.

Streams the public Advanced Trade websocket (no API key) and writes:
  data/live/YYYY-MM-DD/events.csv   top-of-book changes (Q) and trades (T), both legs
  data/live/YYYY-MM-DD/funding.csv  REST poll of the futures product (funding, index)

With --stdout the same rows also go to stdout, for piping into a live consumer.

Needs `pip install websockets`. Times are UTC epoch milliseconds: recv_ms is this
machine's clock when the message arrived, exch_ms is Coinbase's event time.
"""
import argparse
import asyncio
import calendar
import csv
import json
import signal
import sys
import time
import urllib.request
from pathlib import Path

WS_URL = "wss://advanced-trade-ws.coinbase.com"
REST = "https://api.coinbase.com/api/v3/brokerage/market/products/"
ROOT = Path(__file__).resolve().parents[2]
EVENT_COLUMNS = ["recv_ms", "exch_ms", "kind", "product", "bid", "bid_qty", "ask", "ask_qty", "price", "size", "side"]
FUNDING_COLUMNS = ["recv_ms", "product", "price", "funding_rate", "funding_time", "index_price",
                   "open_interest", "future_product_details"]


def now_ms():
    return time.time_ns() // 1_000_000


def parse_time_ms(text):
    """'2026-10-15T20:30:37.167359596Z' -> epoch ms (handles nanosecond fractions)."""
    if not text:
        return 0
    base, _, frac = text.rstrip("Z").partition(".")
    seconds = calendar.timegm(time.strptime(base, "%Y-%m-%dT%H:%M:%S"))
    return seconds * 1000 + int((frac + "000")[:3])


class Book:
    """Price-level book for one product; reports the top of book."""

    def __init__(self):
        self.clear()

    def clear(self):
        self.levels = {"bid": {}, "offer": {}}
        self.best = {"bid": None, "offer": None}

    def update(self, side, price, qty):
        levels, best = self.levels[side], self.best[side]
        better = max if side == "bid" else min
        if qty == 0:
            levels.pop(price, None)
            if price == best:  # Rescan only when the best level disappears.
                self.best[side] = better(levels) if levels else None
        else:
            levels[price] = qty
            if best is None or better(price, best) == price:
                self.best[side] = price

    def top(self):
        bid, ask = self.best["bid"], self.best["offer"]
        if bid is None or ask is None:
            return None
        return bid, self.levels["bid"][bid], ask, self.levels["offer"][ask]


class Writer:
    """Daily-rotating CSV files plus an optional stdout copy of the event stream."""

    def __init__(self, directory, to_stdout):
        self.directory, self.to_stdout = Path(directory), to_stdout
        self.day, self.files = None, {}

    def _file(self, name, columns, ms):
        day = time.strftime("%Y-%m-%d", time.gmtime(ms / 1000))
        if day != self.day:
            self.close()
            self.day = day
        if name not in self.files:
            path = self.directory / day / name
            path.parent.mkdir(parents=True, exist_ok=True)
            new = not path.exists() or path.stat().st_size == 0
            handle = path.open("a", newline="")
            writer = csv.writer(handle)
            if new:
                writer.writerow(columns)
            self.files[name] = (handle, writer)
        return self.files[name]

    def event(self, row):
        handle, writer = self._file("events.csv", EVENT_COLUMNS, row[0])
        writer.writerow(row)
        handle.flush()
        if self.to_stdout:
            sys.stdout.write(",".join(str(v) for v in row) + "\n")
            sys.stdout.flush()

    def funding(self, row):
        handle, writer = self._file("funding.csv", FUNDING_COLUMNS, row[0])
        writer.writerow(row)
        handle.flush()

    def close(self):
        for handle, _ in self.files.values():
            handle.close()
        self.files = {}


def find_key(obj, name):
    """First value stored under `name` anywhere in nested dicts/lists."""
    if isinstance(obj, dict):
        if name in obj:
            return obj[name]
        children = obj.values()
    elif isinstance(obj, list):
        children = obj
    else:
        return None
    for child in children:
        found = find_key(child, name)
        if found is not None:
            return found
    return None


def poll_product(product):
    request = urllib.request.Request(REST + product, headers={"User-Agent": "btc-perps-recorder"})
    with urllib.request.urlopen(request, timeout=10) as response:
        data = json.load(response)
    details = data.get("future_product_details") or {}
    return [now_ms(), product, data.get("price", ""), find_key(details, "funding_rate") or "",
            find_key(details, "funding_time") or "", find_key(data, "index_price") or "",
            find_key(details, "open_interest") or "", json.dumps(details, separators=(",", ":"))]


async def funding_loop(writer, product, seconds):
    while True:
        try:
            writer.funding(await asyncio.to_thread(poll_product, product))
        except Exception as error:  # Network hiccups must not stop the recorder.
            print(f"funding poll failed: {error}", file=sys.stderr)
        await asyncio.sleep(seconds)


class Recorder:
    def __init__(self, writer, products):
        self.writer, self.products = writer, products
        self.books = {p: Book() for p in products}
        self.tops = {}
        self.last_sequence = None

    def handle(self, message, recv_ms):
        """Process one websocket message. Returns False when the stream must resync."""
        sequence = message.get("sequence_num")
        if sequence is not None:
            if self.last_sequence is not None and sequence != self.last_sequence + 1:
                print(f"sequence gap {self.last_sequence} -> {sequence}; resubscribing", file=sys.stderr)
                return False
            self.last_sequence = sequence
        channel = message.get("channel")
        message_ms = parse_time_ms(message.get("timestamp", ""))
        for event in message.get("events", []):
            if channel == "l2_data":
                self._book_event(event, recv_ms, message_ms)
            elif channel == "market_trades":
                for trade in event.get("trades", []):
                    if trade.get("product_id") in self.books:
                        self.writer.event([recv_ms, parse_time_ms(trade.get("time")) or message_ms, "T",
                                           trade["product_id"], "", "", "", "", trade["price"], trade["size"],
                                           trade.get("side", "")])
        return True

    def _book_event(self, event, recv_ms, message_ms):
        product = event.get("product_id")
        book = self.books.get(product)
        if book is None:
            return
        if event.get("type") == "snapshot":
            book.clear()
        exch_ms = message_ms
        for update in event.get("updates", []):
            book.update(update["side"], float(update["price_level"]), float(update["new_quantity"]))
            exch_ms = max(exch_ms, parse_time_ms(update.get("event_time", "")))
        top = book.top()
        if top is not None and top != self.tops.get(product):
            self.tops[product] = top
            bid, bid_qty, ask, ask_qty = top
            self.writer.event([recv_ms, exch_ms, "Q", product, repr(bid), repr(bid_qty),
                               repr(ask), repr(ask_qty), "", "", ""])


async def stream(writer, products):
    import websockets  # Imported here so --help works without the package.

    backoff = 1
    while True:
        recorder = Recorder(writer, products)
        try:
            async with websockets.connect(WS_URL, max_size=None, ping_interval=20) as ws:
                for channel in ("heartbeats", "level2", "market_trades"):
                    await ws.send(json.dumps({"type": "subscribe", "channel": channel, "product_ids": products}))
                print(f"connected; recording {', '.join(products)}", file=sys.stderr)
                backoff = 1
                async for raw in ws:
                    message = json.loads(raw)
                    if message.get("type") == "error":
                        raise RuntimeError(message.get("message", raw))
                    if not recorder.handle(message, now_ms()):
                        break
        except Exception as error:
            print(f"websocket error: {error}; reconnecting in {backoff}s", file=sys.stderr)
        await asyncio.sleep(backoff)
        backoff = min(backoff * 2, 30)


async def main(args):
    writer = Writer(args.output, args.stdout)
    if args.stdout:
        sys.stdout.write(",".join(EVENT_COLUMNS) + "\n")
        sys.stdout.flush()
    tasks = [asyncio.create_task(stream(writer, [args.perp, args.spot]))]
    if args.funding_seconds > 0:
        tasks.append(asyncio.create_task(funding_loop(writer, args.perp, args.funding_seconds)))
    if args.seconds > 0:
        await asyncio.sleep(args.seconds)
    else:
        await asyncio.gather(*tasks)
    for task in tasks:
        task.cancel()
    writer.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--perp", default="BIP-20DEC30-CDE")
    parser.add_argument("--spot", default="BTC-USD")
    parser.add_argument("--output", default=str(ROOT / "data/live"))
    parser.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = run until Ctrl-C)")
    parser.add_argument("--funding-seconds", type=float, default=15, help="REST poll interval (0 = off)")
    parser.add_argument("--stdout", action="store_true", help="also print events to stdout")
    args = parser.parse_args()
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    asyncio.run(main(args))
