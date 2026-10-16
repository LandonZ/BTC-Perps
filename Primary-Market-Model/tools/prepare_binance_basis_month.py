#!/usr/bin/env python3
"""Prepare every day of a UTC month for a continuous tick basis replay."""
import argparse
import json
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path

from download_binance_maker import ROOT


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
    args = parser.parse_args()
    if not 1 <= args.workers <= 8:
        parser.error('workers must be in [1,8]')
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
