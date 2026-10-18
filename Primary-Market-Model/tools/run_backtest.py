#!/usr/bin/env python3
"""Run the current BTC strategy and write one self-contained profit report."""
import argparse
import csv
import json
import math
import os
import re
import subprocess
import tempfile
from datetime import datetime, timezone
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


def render_png(directory, output):
    """Export the minute profit curve; requires matplotlib in the runner's environment."""
    os.environ.setdefault('MPLCONFIGDIR',str(MODEL/'build/matplotlib-cache'))
    os.environ.setdefault('XDG_CACHE_HOME',str(MODEL/'build/cache'))
    Path(os.environ['XDG_CACHE_HOME']).mkdir(parents=True,exist_ok=True)
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.dates as dates
    from matplotlib.ticker import StrMethodFormatter
    s = json.loads((directory/'summary.json').read_text())
    with (directory/'equity.csv').open() as handle:
        rows = list(csv.DictReader(handle))
    times = [datetime.fromtimestamp(int(r['time_ms'])/1000,timezone.utc) for r in rows]
    total = [float(r['net_pnl']) for r in rows]
    realized = [float(r['realized_pnl']) for r in rows]
    if not total or not math.isclose(total[-1],s['net_pnl'],abs_tol=1e-6):
        raise ValueError('PNG final point does not reconcile')
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,
                         'axes.spines.top':False,'axes.spines.right':False,
                         'axes.spines.left':False,'axes.spines.bottom':False})
    fig, ax = plt.subplots(figsize=(12,7.4),dpi=170,facecolor='#fafbfc')
    ax.set_facecolor('#fafbfc')
    fig.subplots_adjust(left=.125,right=.965,bottom=.22,top=.68)
    fig.text(.09,.935,'BTC basis strategy',fontsize=25,fontweight='bold')
    fee_text = f"Maker {s['perp_maker_bps']:g} / perp taker {s['perp_taker_bps']:g} / spot taker {s['spot_taker_bps']:g} bps per fill"
    lower=s.get('entry_bid_basis_bps',s['equilibrium_bps'])
    upper=s.get('entry_ask_basis_bps',s['equilibrium_bps'])
    fig.text(.09,.895,f"Binance BTCUSDT | {times[0]:%Y-%m-%d} to {times[-1]:%Y-%m-%d} UTC | Zero latency",fontsize=10,color='#52656e')
    fig.text(.09,.866,fee_text,fontsize=10,color='#52656e')
    fig.text(.09,.838,f'Entry basis: bid at/below {lower:g} bps; offer at/above {upper:g} bps. Equilibrium: {s["equilibrium_bps"]:g} bps.',fontsize=9,color='#52656e')
    for xpos,label,value in [(.09,'NET PNL',s['net_pnl']),(.405,'EXECUTION FEES',s['fees']),(.72,'REALIZED, INCLUDING FUNDING',s['realized_pnl'])]:
        fig.text(xpos,.81,label+' · USDT',fontsize=9,color='#52656e')
        fig.text(xpos,.758,f'{value:,.2f}' if label=='EXECUTION FEES' else f'{value:+,.2f}',fontsize=22,fontweight='bold')
    ax.plot(times,total,color='#08765f' if s['net_pnl']>=0 else '#b33b4c',lw=1.7,label='Total PnL')
    ax.plot(times,realized,color='#91a0b3',lw=1.2,ls=(0,(4,3)),label='Realized PnL, including funding')
    ax.axhline(0,color='#aab8bf',lw=.7)
    ax.grid(axis='y',color='#dce4e8',lw=.7)
    ax.set_ylabel('Cumulative profit · USDT',labelpad=12)
    ax.yaxis.set_major_formatter(StrMethodFormatter('{x:,.0f}'))
    ax.xaxis.set_major_locator(dates.AutoDateLocator(minticks=4,maxticks=6))
    ax.xaxis.set_major_formatter(dates.DateFormatter('%b %d',tz=timezone.utc))
    ax.tick_params(axis='both',length=0,pad=10)
    ax.set_xlim(times[0],times[-1]); ax.margins(y=.1)
    ax.legend(loc='upper left',frameon=False,ncol=2,fontsize=10)
    fig.text(.09,.127,f"Funding cash: {s['funding_pnl']:+,.2f} USDT | Unrealized PnL: {s['unrealized_pnl']:+,.2f} USDT | Max drawdown: {s['max_drawdown']:,.2f} USDT",fontsize=9,color='#52656e')
    fig.text(.09,.093,f"Peak open BTC per leg: {s['max_open_gross_perp_btc']:,.3f} | Minute equity shown; drawdown measured on every event.",fontsize=9,color='#52656e')
    fig.text(.09,.055,'Funding-only exits; causal premium-minute signal. Unlimited positions. No terminal liquidation; open positions marked at mids.',fontsize=9,color='#52656e')
    fig.text(.09,.031,'Spot trade-price proxy, assumed 1 bp full spread. Recorded funding. Borrow, impact, depth, margin and liquidation omitted.',fontsize=9,color='#52656e')
    fig.savefig(output,facecolor=fig.get_facecolor())
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=MODEL/'results/result.html')
    parser.add_argument('--from-results',type=Path,help='Render an existing result without rerunning it')
    parser.add_argument('--events',type=Path,action='append')
    parser.add_argument('--events-list',type=Path)
    parser.add_argument('--premium-events',type=Path)
    parser.add_argument('--png',action='store_true',help='Also export the profit curve to PNG (requires matplotlib)')
    args, options = parser.parse_known_args()
    if args.from_results:
        if options or args.events or args.events_list or args.premium_events:
            parser.error('--from-results renders saved results; it does not accept replay options')
        summary = render_report(args.from_results,args.output)
        if args.png: render_png(args.from_results,args.output.with_suffix('.png'))
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
            if args.png: render_png(Path(work),args.output.with_suffix('.png'))
    print(f'Profit: {summary["net_pnl"]:+,.2f} USDT; realized: {summary["realized_pnl"]:+,.2f} USDT')
    print(f'Report: {args.output.resolve()}')


if __name__ == '__main__':
    main()
