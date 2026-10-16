#!/usr/bin/env python3
"""Download checksum-verified Binance archives and prepare a causal maker replay.

Default: one hour on 2023-07-20, a date with archived BTCUSDT bookTicker data.
The entire daily ZIPs are retained; --hours controls the prepared replay window.
Default spot reference is the most recent spot trade, explicitly a midpoint proxy.
The older completed-minute-close reference remains available as a comparison.
"""
import argparse
import calendar
import csv
import hashlib
import heapq
import io
import json
import os
import urllib.request
import zipfile
import tempfile
from contextlib import ExitStack
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = 'https://data.binance.vision/data/'
COLUMNS = ['time_ms', 'kind', 'price', 'quantity', 'bid', 'ask', 'buyer_maker', 'funding_rate', 'interval_hours']


def download(relative, directory):
    path = directory / Path(relative).name
    directory.mkdir(parents=True, exist_ok=True)
    url = BASE + relative
    with urllib.request.urlopen(url + '.CHECKSUM', timeout=30) as response:
        expected = response.read().decode().split()[0]
    actual = None
    if path.exists():
        with path.open('rb') as handle:
            actual = hashlib.file_digest(handle, 'sha256').hexdigest()
    if actual != expected:
        temporary = path.with_suffix('.zip.tmp')
        try:
            print(f'Downloading {url}', flush=True)
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as output:
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
            with temporary.open('rb') as handle:
                actual = hashlib.file_digest(handle, 'sha256').hexdigest()
            if actual != expected:
                raise RuntimeError(f'Checksum mismatch: {path.name}')
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    print(f'Verified {path.name} ({path.stat().st_size:,} bytes)', flush=True)
    return path, {'url': url, 'path': str(path.relative_to(ROOT)), 'sha256': expected, 'bytes': path.stat().st_size}


def rows(path):
    with zipfile.ZipFile(path) as archive:
        names = [name for name in archive.namelist() if name.endswith('.csv')]
        if len(names) != 1:
            raise ValueError(f'Expected one CSV in {path}')
        with archive.open(names[0]) as handle:
            for row in csv.reader(io.TextIOWrapper(handle)):
                if row and row[0].isdigit():
                    yield row


def millis(value):
    timestamp = int(value)
    return timestamp // 1000 if timestamp > 10**14 else timestamp


def event(timestamp, kind, price='', quantity='', bid='', ask='', buyer='', rate='', interval=''):
    return [timestamp, kind, price, quantity, bid, ask, buyer, rate, interval]


def normalized_events(paths, start, end):
    """Stream normalized rows; sorting and feed validation happen in prepare()."""
    for row in rows(paths['book']):
        timestamp = millis(row[5]) # transaction_time, NOT event_time or update ID.
        if start <= timestamp < end:
            yield event(timestamp, 'Q', bid=row[1], ask=row[3])
    for row in rows(paths['trades']):
        timestamp = millis(row[5])
        if start <= timestamp < end:
            yield event(timestamp, 'T', row[1], row[2], buyer=row[6].lower())
    if 'spot_trades' in paths:
        prior_spot = None
        for row in rows(paths['spot_trades']):
            timestamp = millis(row[5])
            if timestamp < start and (prior_spot is None or timestamp > prior_spot[0]):
                prior_spot = event(timestamp,'S',row[1])
            elif start <= timestamp < end:
                yield event(timestamp,'S',row[1])
        if prior_spot:
            yield prior_spot
    else:
        for row in rows(paths['spot']):
            timestamp = millis(row[6]) + 1 # close_time is inclusive.
            if start <= timestamp < end:
                yield event(timestamp, 'S', row[4])
    marks = {millis(row[0]): float(row[1]) for row in rows(paths['mark'])}
    prior = None
    for row in rows(paths['funding']):
        timestamp = millis(row[0])
        if timestamp < start and (prior is None or timestamp > prior[0]):
            prior = event(timestamp, 'R', rate=row[2], interval=row[1])
        elif start <= timestamp < end:
            mark_time = timestamp//60000*60000
            if mark_time not in marks:
                raise ValueError(f'Missing settlement mark candle at {timestamp}')
            yield event(timestamp, 'F', marks[mark_time], rate=row[2], interval=row[1])
    if prior:
        yield prior


def sorted_events(events, directory, chunk_rows=250000, fan_in=32):
    """Stable external merge sort: bound RAM and open files for full-day feeds."""
    if chunk_rows < 1 or fan_in < 2:
        raise ValueError('Invalid external-sort chunk size or fan-in')
    priority = {'R': 0, 'F': 0, 'S': 1, 'Q': 2, 'T': 3}
    def key(row):
        return int(row[0]), priority[row[1]]
    chunks, buffer = [], []
    def flush():
        path = directory / f'chunk-{len(chunks)}.csv'
        buffer.sort(key=key)
        with path.open('w', newline='') as handle:
            csv.writer(handle).writerows(buffer)
        chunks.append(path)
        buffer.clear()
    for row in events:
        buffer.append(row)
        if len(buffer) >= chunk_rows:
            flush()
    if buffer:
        flush()
    generation = 0
    while len(chunks) > 1:
        merged = []
        for offset in range(0, len(chunks), fan_in):
            group = chunks[offset:offset+fan_in]
            path = directory / f'merge-{generation}-{offset}.csv'
            with ExitStack() as stack, path.open('w', newline='') as output:
                readers = [csv.reader(stack.enter_context(source.open(newline=''))) for source in group]
                csv.writer(output).writerows(heapq.merge(*readers, key=key))
            for source in group:
                source.unlink()
            merged.append(path)
        chunks = merged
        generation += 1
    if chunks:
        with chunks[0].open(newline='') as handle:
            yield from csv.reader(handle)


def prepare(paths, start, end, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.csv.tmp')
    counts = {}
    try:
        with tempfile.TemporaryDirectory(prefix='binance-sort-', dir=output.parent) as workspace:
            ordered = sorted_events(normalized_events(paths, start, end), Path(workspace))
            with temporary.open('w', newline='') as handle:
                writer = csv.writer(handle)
                writer.writerow(COLUMNS)
                for row in ordered:
                    writer.writerow(row)
                    counts[row[1]] = counts.get(row[1], 0)+1
        if any(not counts.get(kind) for kind in ('Q', 'T', 'S')) or not (counts.get('R') or counts.get('F')):
            raise ValueError('Missing book, trades, spot or funding for the replay window')
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return counts


def prepare_premium(month, output):
    day = datetime.strptime(month, '%Y-%m').replace(tzinfo=timezone.utc)
    start = int(day.timestamp()*1000)
    archive, source = download(f'futures/um/monthly/premiumIndexKlines/BTCUSDT/1m/BTCUSDT-1m-{month}.zip',
                               ROOT/'data/binance/maker'/month/'archives/premium')
    observations = [(millis(row[6])+1,row[4]) for row in rows(archive)]
    observations.sort()
    expected = calendar.monthrange(day.year,day.month)[1]*1440
    if len(observations) != expected or any(t != start+(i+1)*60000 for i,(t,_) in enumerate(observations)):
        raise ValueError('Premium-index candles must have complete chronological minute coverage')
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('w',newline='') as handle:
        writer=csv.writer(handle);writer.writerow(COLUMNS)
        writer.writerows(event(t,'P',price) for t,price in observations)
    output.with_suffix('.manifest.json').write_text(json.dumps(dict(source=source,count=len(observations),
        availability='Completed 1m premium index CLOSE at close_time+1ms; causal proxy for exchange 5s samples'),indent=2)+'\n')
    print(f'Prepared premium events: {output} ({len(observations)} minutes)',flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--date', default='2023-07-20', help='UTC day; recent bookTicker archives may be unavailable')
    parser.add_argument('--start-hour', type=int, default=0)
    parser.add_argument('--hours', type=int, default=1)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--spot-reference', choices=['last-trade','minute-close'], default='last-trade')
    parser.add_argument('--premium-month', help='Prepare only completed premium-index minutes for YYYY-MM')
    args = parser.parse_args()
    if args.premium_month:
        prepare_premium(args.premium_month,args.output or ROOT/'data/binance/maker'/args.premium_month/'premium-events.csv')
        return
    if not 0 <= args.start_hour < 24 or not 1 <= args.hours <= 24-args.start_hour:
        parser.error('Window must fit within one UTC day')
    day = datetime.strptime(args.date, '%Y-%m-%d').replace(tzinfo=timezone.utc)
    start = int(day.timestamp()*1000)+args.start_hour*3600000
    end = start+args.hours*3600000
    directory = ROOT / 'data/binance/maker' / args.date
    output = args.output or directory / ('events-spot-trades.csv' if args.spot_reference == 'last-trade' else 'events.csv')
    archive_dir = directory / 'archives'
    sources = {
        'book': f'futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-{args.date}.zip',
        'trades': f'futures/um/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-{args.date}.zip',
        'spot': f'spot/daily/klines/BTCUSDT/1m/BTCUSDT-1m-{args.date}.zip',
        'mark': f'futures/um/daily/markPriceKlines/BTCUSDT/1m/BTCUSDT-1m-{args.date}.zip',
        'funding': f'futures/um/monthly/fundingRate/BTCUSDT/BTCUSDT-fundingRate-{args.date[:7]}.zip',
    }
    if args.spot_reference == 'last-trade':
        del sources['spot']
        sources['spot_trades'] = f'spot/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-{args.date}.zip'
    paths, manifest = {}, {}
    for name, relative in sources.items():
        # Spot and mark klines have IDENTICAL archive basenames.
        paths[name], manifest[name] = download(relative, archive_dir/name if name in ('spot', 'mark','spot_trades') else archive_dir)
    counts = prepare(paths, start, end, output)
    metadata = {'symbol': 'BTCUSDT', 'venue': 'Binance USD-M perpetual', 'start_ms': start, 'end_ms': end,
                'events': str(output), 'counts': counts, 'sources': manifest,
                'spot_reference': 'Latest observed spot trade; proxy, NOT an observed spot midpoint' if args.spot_reference == 'last-trade' else 'Previous completed spot 1m close, available at close_time+1ms; midpoint proxy',
                'funding_signal': 'Last settled rate only; final next-settlement rate is never used early',
                'funding_mark': 'Settlement minute mark-price candle OPEN; proxy for official funding mark',
                'book_clock': 'Historical exchange transaction_time; receive-clock latency is simulated',
                'ordering': 'Same millisecond: funding, spot, book, trades; strategy decides after batch'}
    output.with_suffix('.manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(f'Prepared {output}: {counts}', flush=True)


if __name__ == '__main__':
    main()
