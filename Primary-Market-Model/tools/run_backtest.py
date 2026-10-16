#!/usr/bin/env python3
"""Run the current BTC strategy and write one self-contained profit report."""
import argparse
import csv
import json
import math
import re
import subprocess
import tempfile
from pathlib import Path

MODEL = Path(__file__).resolve().parents[1]
ROOT = MODEL.parent


def render_report(directory, output):
    summary = json.loads((directory/'summary.json').read_text())
    with (directory/'equity.csv').open() as handle:
        points = [[int(row['time_ms']),float(row['net_pnl']),float(row['realized_pnl'])]
                  for row in csv.DictReader(handle)]
    if not points or not math.isclose(points[-1][1],summary['net_pnl'],abs_tol=1e-6):
        raise ValueError('Final graph value does not reconcile with the result')
    if any(not all(math.isfinite(v) for v in row) for row in points):
        raise ValueError('Nonfinite graph values')
    if any(a[0] > b[0] for a,b in zip(points,points[1:])):
        raise ValueError('Profit observations must be chronological')
    # Keep both extrema in each display bucket, as well as its endpoints.
    # Values remain original observations rather than averaged profits.
    step = max(1,math.ceil(len(points)/1600))
    selected = []
    for offset in range(0,len(points),step):
        bucket = points[offset:offset+step]
        indices = {0,len(bucket)-1}
        for column in (1,2):
            indices.add(min(range(len(bucket)),key=lambda i:bucket[i][column]))
            indices.add(max(range(len(bucket)),key=lambda i:bucket[i][column]))
        selected.extend(bucket[i] for i in sorted(indices))
    payload = json.dumps(dict(summary=summary,points=selected),separators=(',',':')).replace('<','\\u003c')
    template = Path(__file__).with_name('report.html').read_text()
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(template.replace('__REPORT_DATA__',payload))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=MODEL/'results/result.html')
    parser.add_argument('--from-results',type=Path,help='Render an existing result without rerunning it')
    parser.add_argument('--events',type=Path,action='append')
    parser.add_argument('--events-list',type=Path)
    parser.add_argument('--premium-events',type=Path)
    args, options = parser.parse_known_args()
    if args.from_results:
        if options or args.events or args.events_list or args.premium_events:
            parser.error('--from-results renders saved results; it does not accept replay options')
        summary = render_report(args.from_results,args.output)
    else:
        binary = MODEL/'build/basis-maker'
        subprocess.run(['make','-C',str(MODEL),'build/basis-maker'],check=True)
        inputs = []
        for path in args.events or []:
            inputs.extend(['--events',str(path.resolve())])
        if args.events_list:
            inputs.extend(['--events-list',str(args.events_list.resolve())])
        if not inputs:
            sample = ROOT/'data/binance/maker/2023-07-20/events-spot-trades.csv'
            if not sample.exists():
                parser.error('Prepare data first: python3 tools/download_binance_maker.py (from Primary-Market-Model)')
            inputs = ['--events',str(sample)]
        premium = args.premium_events
        if premium is None:
            example = str(args.events[0]) if args.events else (args.events_list.read_text().splitlines()[0] if args.events_list else str(sample))
            match = re.search(r'20\d{2}-\d{2}',example)
            if not match: parser.error('Supply --premium-events when the input path does not identify a month')
            month = match.group(0)
            premium = ROOT/'data/binance/maker'/month/'premium-events.csv'
            if not premium.exists():
                subprocess.run(['python3',str(MODEL/'tools/download_binance_maker.py'),'--premium-month',month],check=True)
        inputs.extend(['--premium-events',str(premium.resolve())])
        with tempfile.TemporaryDirectory(prefix='btc-basis-result-') as work:
            subprocess.run([str(binary),*inputs,*options,'--details','0','--output',work],check=True)
            summary = render_report(Path(work),args.output)
    print(f'Profit: {summary["net_pnl"]:+,.2f} USDT; realized: {summary["realized_pnl"]:+,.2f} USDT')
    print(f'Report: {args.output.resolve()}')


if __name__ == '__main__':
    main()
