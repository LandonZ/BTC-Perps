#!/usr/bin/env python3
"""Plot entry-deviation sensitivity on five distinct randomly selected UTC days.

Uses the single-day sweep's fees, funding exits, signed position limit and
accounting checks. Each day and threshold starts flat. Monthly ZIPs are scanned
once per source, retaining only the randomly selected days.
"""
import argparse
import csv
import json
import os
import random
import secrets
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

from sweep_entry_deviation import (MODEL, ROOT, DAY, available_days, deviation_grid,
                                   extract_day, replay_day, sha256)
from presentation import setup, style_axis, SURFACE, INK, MUTED


def select_days(candidates, seed, count):
    if count<1 or count>len(candidates):raise ValueError('Day count exceeds available distinct days')
    return sorted(random.Random(seed).sample(sorted(candidates),count))


def render(series, output, exit_on_opposite_signal=False):
    plt = setup()
    from matplotlib.ticker import FuncFormatter, MultipleLocator
    fig = plt.figure(figsize=(16,9),dpi=240,facecolor=SURFACE)
    fig.text(.08,.92,'Net PnL vs entry deviation',fontsize=29,weight='bold',color=INK)
    fig.text(.08,.867,f'BTCUSDT · {len(series)} UTC days · Flat start for every day and threshold',fontsize=13,color=MUTED)
    fig.text(.08,.823,'Equilibrium −5 bp   |   Maker 0.5 bp · Taker 2 bp   |   Position −1 / +1 · 0.01 BTC per trade',
             fontsize=11,color=MUTED)
    if exit_on_opposite_signal:
        fig.text(.08,.782,'Opposite entry signal → flat   ·   Funding-payer exits retained',fontsize=11,color=MUTED)
    ax = fig.add_axes([.10,.15,.82,.56]);style_axis(ax)
    colors=['#087f6b','#3568a8','#d17b22','#9c529f','#c44955','#5d7885','#7f8530']
    markers=['o','s','^','D','v','P','X']
    for index,(day,points) in enumerate(sorted(series.items())):
        x=[row['deviation_bps'] for row in points];y=[row['net_pnl_usdt'] for row in points]
        ax.plot(x,y,label=day,color=colors[index%len(colors)],lw=1.9,
                marker=markers[index%len(markers)],markersize=4.5)
    ax.legend(loc='lower left',bbox_to_anchor=(0,1.035),ncol=min(len(series),5),
              frameon=False,fontsize=11,columnspacing=1.8,handlelength=2.2,borderaxespad=0)
    ax.set_xlabel('Entry distance from equilibrium · bp',fontsize=13,labelpad=15)
    ax.set_ylabel('Net PnL · USDT',fontsize=13,labelpad=15)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda value,_:f'{value:,.2f}'))
    ax.xaxis.set_major_locator(MultipleLocator(.5 if x[-1]-x[0]<=5 else 1))
    ax.margins(x=.03,y=.13)
    # Preserve the common linear scale while making smaller curves readable
    # when one day's PnL is much larger than the others.
    if len(series)>2:
        dominant=max(series,key=lambda day:max(abs(row['net_pnl_usdt']) for row in series[day]))
        other_values=[row['net_pnl_usdt'] for day,points in series.items() if day!=dominant for row in points]
        all_values=[row['net_pnl_usdt'] for points in series.values() for row in points]
        if max(all_values)-min(all_values)>6*max(max(other_values)-min(other_values),.01):
            detail=ax.inset_axes([.53,.32,.44,.43]);style_axis(detail)
            for index,(day,points) in enumerate(sorted(series.items())):
                if day==dominant:continue
                detail.plot([row['deviation_bps'] for row in points],[row['net_pnl_usdt'] for row in points],
                            color=colors[index%len(colors)],lw=1.5,marker=markers[index%len(markers)],markersize=3)
            detail.set_title(f'Other {len(series)-1} days · expanded scale',loc='left',fontsize=10,pad=10)
            detail.tick_params(labelsize=9,pad=5)
            detail.yaxis.set_major_formatter(FuncFormatter(lambda value,_:f'{value:,.1f}'))
            detail.set_xticks([x[0],(x[0]+x[-1])/2,x[-1]])
            detail.set_xlabel('Entry distance · bp',fontsize=9,labelpad=5)
            detail.set_ylabel('USDT',fontsize=9,labelpad=5)
            detail.margins(x=.03,y=.18)
    fig.savefig(output,facecolor=SURFACE);plt.close(fig)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--days',type=int,default=5)
    parser.add_argument('--dates',nargs='+',help='Explicit distinct UTC dates instead of a new random sample')
    parser.add_argument('--seed',type=int,help='Generated and recorded when omitted')
    parser.add_argument('--start-bps',default='1')
    parser.add_argument('--stop-bps',default='5')
    parser.add_argument('--step-bps',default='0.25')
    parser.add_argument('--exit-on-opposite-signal',action='store_true',help='Close to flat on the opposite entry band; wait for a fresh episode to reopen')
    parser.add_argument('--output',type=Path,default=MODEL/'results/entry-deviation-five-days')
    args=parser.parse_args()
    deviations=deviation_grid(args.start_bps,args.stop_bps,args.step_bps)
    if deviations[-1]>=9995:parser.error('Entry distance would produce an invalid basis threshold')
    candidates=available_days();seed=args.seed if args.seed is not None else secrets.randbits(64)
    days=sorted(args.dates) if args.dates else select_days(candidates,seed,args.days)
    if not days or len(set(days))!=len(days) or any(day not in candidates for day in days):
        parser.error('Dates must be distinct, available archived UTC days')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    if (output/'selection.json').exists():parser.error('Output already contains a sweep; choose a new --output')
    selection=dict(days=days,seed=seed,candidates=sorted(candidates),deviations_bps=deviations,
                   selection='Explicit UTC dates' if args.dates else 'Uniform random sample without replacement, chosen before inspecting PnL')
    (output/'selection.json').write_text(json.dumps(selection,indent=2)+'\n')
    print(f'Selected days: {", ".join(days)}; seed={seed}',flush=True)
    manifests={candidates[day]['month']:candidates[day] for day in days}
    for month,manifest in sorted(manifests.items()):
        for name,source in manifest['sources'].items():
            if sha256(ROOT/source['path'])!=source['sha256']:raise ValueError(f'{month} {name}: checksum mismatch')
        print(f'{month}: source checksums verified',flush=True)
    subprocess.run(['make','-C',str(MODEL),'build/archive-replay','build/archive-sort'],check=True)
    # Keep plotting caches in the writable build directory.
    cache=MODEL/'build/plot-cache';cache.mkdir(exist_ok=True)
    os.environ.setdefault('MPLCONFIGDIR',str(cache/'matplotlib'))
    os.environ.setdefault('XDG_CACHE_HOME',str(cache))
    provenance=dict(**selection,sources=manifests,
                    engine_sha256=sha256(MODEL/'build/archive-replay'),
                    archive_sort_sha256=sha256(MODEL/'build/archive-sort'),
                    script_sha256=sha256(Path(__file__).resolve()),
                    single_day_script_sha256=sha256(Path(__file__).with_name('sweep_entry_deviation.py')),
                    equilibrium_bps=-5,position_limit_trades=[-1,1],trade_btc=.01,
                    fees_bps=dict(perp_maker=.5,perp_taker=2,spot_taker=2),
                    pnl_definition='Final marked equity including funding and executed fees; independent flat starts; no terminal liquidation',
                    exit_on_opposite_signal=args.exit_on_opposite_signal,
                    exits=('Opposite entry band closes to flat at taker prices; consume that signal episode; ' if args.exit_on_opposite_signal else '')+
                          'anticipated funding payers also exit in the final 60 seconds before settlement using a causal premium forecast',
                    sorted_selected_days_sha256={})
    series={}
    with tempfile.TemporaryDirectory(prefix='five-day-sweep-',dir=MODEL/'build') as temporary:
        folder=Path(temporary)
        with ThreadPoolExecutor(max_workers=6) as pool:
            jobs={}
            for month,manifest in sorted(manifests.items()):
                starts=[int(datetime.strptime(day,'%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp()*1000)
                        for day in days if day.startswith(month)]
                jobs[month]=[pool.submit(extract_day,kind,name,manifest,folder/month,
                                         min(starts),max(starts)+DAY,starts,folder)
                             for kind,name in [('Q','book'),('T','trades'),('S','spot_trades')]]
            for month,manifest in sorted(manifests.items()):
                extracted=[job.result() for job in jobs[month]]
                provenance['sorted_selected_days_sha256'][month]={kind:digest for kind,_,digest in extracted}
                for day in days:
                    if not day.startswith(month):continue
                    directory=output/'days'/day
                    series[day]=replay_day(day,manifest,deviations,directory,extracted,args.exit_on_opposite_signal)
                    provenance.setdefault('funding_premium_sha256',{})[day]=sha256(directory/'funding-premium.csv')
                    print(f'Completed {day}: all {len(deviations)} thresholds validated',flush=True)
                (output/'inputs.json').write_text(json.dumps(provenance,indent=2)+'\n')
    records=[dict(day=day,**point) for day,points in sorted(series.items()) for point in points]
    with (output/'sweep.csv').open('w',newline='') as handle:
        writer=csv.DictWriter(handle,fieldnames=list(records[0]));writer.writeheader();writer.writerows(records)
    (output/'sweep.json').write_text(json.dumps(dict(**selection,series=series),indent=2)+'\n')
    render(series,output/'pnl-vs-deviation.png',args.exit_on_opposite_signal)
    print(f'Saved {output/"pnl-vs-deviation.png"}',flush=True)


if __name__=='__main__':main()
