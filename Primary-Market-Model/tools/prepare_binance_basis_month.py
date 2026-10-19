#!/usr/bin/env python3
"""Prepare every day of a UTC month for a continuous tick basis replay."""
import argparse
import json
import subprocess
import sys
import time
import urllib.error
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta, timezone
from pathlib import Path

from download_binance_maker import ROOT, download, rows, millis


def archive_month(month, workers):
    """Fetch compact monthly inputs without retaining a large sorted tick CSV."""
    begin = datetime.strptime(month, '%Y-%m').replace(tzinfo=timezone.utc)
    end = (begin.replace(day=28)+timedelta(days=4)).replace(day=1)
    directory = ROOT/'data/binance/maker'/month
    sources = {
        'book': f'futures/um/monthly/bookTicker/BTCUSDT/BTCUSDT-bookTicker-{month}.zip',
        'trades': f'futures/um/monthly/aggTrades/BTCUSDT/BTCUSDT-aggTrades-{month}.zip',
        'spot_trades': f'spot/monthly/aggTrades/BTCUSDT/BTCUSDT-aggTrades-{month}.zip',
        'mark': f'futures/um/monthly/markPriceKlines/BTCUSDT/1m/BTCUSDT-1m-{month}.zip',
        'funding': f'futures/um/monthly/fundingRate/BTCUSDT/BTCUSDT-fundingRate-{month}.zip',
        'premium': f'futures/um/monthly/premiumIndexKlines/BTCUSDT/1m/BTCUSDT-1m-{month}.zip',
    }
    manifest = dict(symbol='BTCUSDT', month=month, start_ms=int(begin.timestamp()*1000),
                    end_ms=int(end.timestamp()*1000), sources={},
                    status='downloading',
                    usage='Monthly source ZIPs for continuous archive replay or reference CSV conversion. Spot trades remain a midpoint proxy.')
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory/'month-inputs.manifest.json'
    def save():
        temporary = destination.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(manifest, indent=2)+'\n')
        temporary.replace(destination)
    save()
    def fetch(relative, destination):
        for attempt in range(4):
            try: return download(relative, destination, 8)
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                if attempt == 3: raise
                time.sleep(2**attempt)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        pending = {pool.submit(fetch, relative, directory/'archives'/name): name
                   for name,relative in sources.items()}
        for future in as_completed(pending):
            name = pending[future]
            _, manifest['sources'][name] = future.result()
            save()
            print(f'Completed {month} {name}: {len(manifest["sources"])}/{len(sources)} archives', flush=True)
    premium_times = [millis(row[6])+1 for row in rows(ROOT/manifest['sources']['premium']['path'])]
    if premium_times != list(range(manifest['start_ms']+60000, manifest['end_ms']+1, 60000)):
        raise ValueError('Premium-index archive does not have complete chronological minute coverage')
    mark_times = [millis(row[0]) for row in rows(ROOT/manifest['sources']['mark']['path'])]
    if mark_times != list(range(manifest['start_ms'], manifest['end_ms'], 60000)):
        raise ValueError('Mark-price archive does not have complete chronological minute coverage')
    funding = list(rows(ROOT/manifest['sources']['funding']['path']))
    funding_times = [millis(row[0]) for row in funding]
    if not funding_times or funding_times != sorted(set(funding_times)) or any(
            not manifest['start_ms'] <= t < manifest['end_ms'] for t in funding_times):
        raise ValueError('Funding records must have unique chronological timestamps within the month')
    manifest['coverage_checks'] = dict(mark_minutes=len(mark_times), funding_settlements=len(funding),
        premium_minutes=len(premium_times),
        tick_coverage='Not exhaustively scanned; quote/trade gaps must be checked during replay preparation')
    manifest['status'] = 'checksum_verified'
    manifest['downloaded_bytes'] = sum(source['bytes'] for source in manifest['sources'].values())
    save()
    print(f'Monthly inputs: {destination}', flush=True)


def prepare_day(day):
    output = ROOT/'data/binance/maker'/day/'events-spot-trades-full-day.csv'
    manifest = output.with_suffix('.manifest.json')
    if output.exists() and manifest.exists():
        metadata = json.loads(manifest.read_text())
        if metadata['end_ms']-metadata['start_ms'] == 86400000:
            return str(output), metadata['counts']
    subprocess.run([sys.executable, str(Path(__file__).with_name('download_binance_maker.py')),
                    '--date', day, '--hours', '24', '--output', str(output)], check=True)
    return str(output), json.loads(manifest.read_text())['counts']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--month', required=True)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--archives-only', action='store_true',
                        help='Download and validate compact monthly source ZIPs without generating tick CSVs')
    args = parser.parse_args()
    if not 1 <= args.workers <= 8:
        parser.error('workers must be in [1,8]')
    if args.archives_only:
        archive_month(args.month, args.workers)
        return
    begin = datetime.strptime(args.month, '%Y-%m')
    end = (begin.replace(day=28)+timedelta(days=4)).replace(day=1)
    days = [(begin+timedelta(days=n)).strftime('%Y-%m-%d') for n in range((end-begin).days)]
    results = {}
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        pending = {pool.submit(prepare_day, day): day for day in days}
        for future in as_completed(pending):
            day = pending[future]
            results[day] = future.result()
            print(f'Completed {day}: {len(results)}/{len(days)} days', flush=True)
    output = ROOT/'data/binance/maker'/args.month/'events-list.txt'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(''.join(results[day][0]+'\n' for day in days))
    output.with_suffix('.json').write_text(json.dumps(results, indent=2)+'\n')
    print(f'Continuous replay list: {output}', flush=True)


if __name__ == '__main__':
    main()
