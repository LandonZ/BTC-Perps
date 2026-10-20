#!/usr/bin/env python3
"""Replay one randomly selected UTC day at entry deviations of 1 to 5 bp.

Run with build/trial-venv/bin/python tools/sweep_entry_deviation.py.
Use --seed to reproduce random selection, --day YYYY-MM-DD to choose a day,
or --stop-bps to extend the sweep. Every run starts flat on identical ticks.
"""
import argparse
import csv
import hashlib
import json
import math
import random
import secrets
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

from download_binance_maker import ROOT, COLUMNS, event, rows, millis
from run_archive_backtest import MODEL, verify_funding, verify_sides
from presentation import setup, style_axis, SURFACE, INK, MUTED, GREEN

DAY = 86400000
SIDES = ('long_perp_short_spot', 'short_perp_long_spot')


def deviation_grid(start, stop, step):
    start, stop, step = map(lambda x: Decimal(str(x)), (start, stop, step))
    if not all(x.is_finite() for x in (start, stop, step)) or start<=0 or step<=0 or stop<start:
        raise ValueError('Require finite 0 < start <= stop and step > 0')
    count = (stop-start)/step
    if count != count.to_integral_value():
        raise ValueError('Stop must fall on the requested step grid')
    if count>1000:raise ValueError('At most 1,001 deviations per sweep')
    return [float(start+i*step) for i in range(int(count)+1)]


def sha256(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def available_days():
    candidates = {}
    for path in sorted((ROOT/'data/binance/maker').glob('*/month-inputs.manifest.json')):
        manifest = json.loads(path.read_text())
        required = {'book', 'trades', 'spot_trades', 'funding', 'mark', 'premium'}
        if manifest.get('status') != 'checksum_verified' or not required.issubset(manifest['sources']):continue
        if not all((ROOT/manifest['sources'][name]['path']).is_file() for name in required):continue
        for start in range(manifest['start_ms'], manifest['end_ms'], DAY):
            candidates[datetime.fromtimestamp(start/1000,timezone.utc).strftime('%Y-%m-%d')] = manifest
    if not candidates:raise ValueError('No complete archived days available')
    return candidates


def extract_day(kind, name, manifest, folder, start, end, selected_starts=None, slot_folder=None):
    """Scan a monthly ZIP once, retain the day, and exactly sort timestamp ties."""
    binary = folder/(name+'.bin')
    folder.mkdir(parents=True,exist_ok=True)
    archive = ROOT/manifest['sources'][name]['path']
    with binary.open('wb') as destination, (folder/(name+'.log')).open('w') as log:
        unzip = subprocess.Popen(['unzip', '-p', str(archive)], stdout=subprocess.PIPE, stderr=log)
        try:
            command = [str(MODEL/'build/archive-sort'), kind, str(folder/('runs-'+name)),
                       str(slot_folder or folder), '4000000', str(start), str(end)]
            if selected_starts is not None:command.append(','.join(map(str,selected_starts)))
            subprocess.run(command,
                           stdin=unzip.stdout, stdout=destination, stderr=log, check=True)
            unzip.stdout.close()
            if unzip.wait()!=0:raise RuntimeError(f'{name}: decompression failed')
        finally:
            unzip.stdout.close()
            if unzip.poll() is None:unzip.terminate()
            unzip.wait()
    print(f'Extracted and sorted {manifest["month"]} {name}: {(binary.stat().st_size-16)//32:,} events',flush=True)
    return kind, binary, sha256(binary)


def small_events(manifest, start, end):
    result = [event(start-8*3600000, 'R', rate=0, interval=8)]
    marks = {millis(row[0]):row[1] for row in rows(ROOT/manifest['sources']['mark']['path'])}
    for row in rows(ROOT/manifest['sources']['funding']['path']):
        t = millis(row[0])
        if start<=t<end:result.append(event(t,'F',marks[t//60000*60000],rate=row[2],interval=row[1]))
    for row in rows(ROOT/manifest['sources']['premium']['path']):
        t = millis(row[6])+1
        if start<=t<end:result.append(event(t,'P',row[4]))
    return sorted(result,key=lambda row:(int(row[0]),0 if row[1] in ('R','F') else 1))


def validate_run(directory, summary, manifest, start, end, deviation, exit_on_opposite_signal=False):
    expected = dict(equilibrium_bps=-5,entry_offset_bps=deviation,perp_maker_bps=.5,
                    perp_taker_bps=2,spot_taker_bps=2,order_btc=.01,
                    one_order_per_entry_event=True,single_basis_position=True,
                    exit_on_equilibrium=False,exit_before_funding=True,funding_exit_payers_only=True,
                    exit_on_opposite_signal=exit_on_opposite_signal)
    for key, value in expected.items():
        if summary[key]!=value:raise ValueError(f'Unexpected setting {key}: {summary[key]}')
    if (summary['max_open_gross_perp_btc']>.01+1e-9 or summary['max_entry_event_btc']>.01+1e-9
            or summary['entry_band_violations'] or summary['convergence_exit_pairs']
            or abs(summary['delta_btc'])>1e-9):raise ValueError('Position, entry, exit or hedge invariant failed')
    expected_fees = sum(summary[leg+'_notional']*summary[leg+'_bps']/10000
                        for leg in ('perp_maker','perp_taker','spot_taker'))
    if not math.isclose(summary['fees'],expected_fees,abs_tol=1e-6):raise ValueError('Fee mismatch')
    if not math.isclose(summary['net_pnl'],summary['price_pnl']+summary['funding_pnl']-summary['fees'],
                        abs_tol=1e-6):raise ValueError('PnL mismatch')
    if summary['closed_pairs']!=summary['funding_exit_pairs']+summary['opposite_signal_exit_pairs']+summary.get('daily_trial_exit_pairs',0):
        raise ValueError('Closed positions do not reconcile to enabled exit rules')
    with (directory/'equity.csv').open() as handle:
        for row in csv.DictReader(handle):
            if not start<=int(row['time_ms'])<end:raise ValueError('Equity outside selected day')
            if min(float(row[side+'_open_btc']) for side in SIDES)>1e-9:
                raise ValueError('Simultaneous opposite positions')
    validation = verify_funding(directory,[manifest],start,end)
    validation.update(verify_sides(directory,summary))
    validation['max_position_units'] = summary['max_open_gross_perp_btc']/.01
    (directory/'validation.json').write_text(json.dumps(validation,indent=2)+'\n')


def render(points, day, output, exit_on_opposite_signal=False):
    plt = setup()
    from matplotlib.ticker import FuncFormatter, MultipleLocator
    fig = plt.figure(figsize=(16,9),dpi=240,facecolor=SURFACE)
    fig.text(.08,.92,'Net PnL vs entry deviation',fontsize=29,weight='bold',color=INK)
    fig.text(.08,.867,f'BTCUSDT · {day} UTC · Same day, flat start for each threshold',fontsize=13,color=MUTED)
    fig.text(.08,.823,'Equilibrium −5 bp   |   Maker 0.5 bp · Taker 2 bp   |   Position −1 / +1 · 0.01 BTC per trade',
             fontsize=11,color=MUTED)
    if exit_on_opposite_signal:
        fig.text(.08,.782,'Opposite entry signal → flat   ·   Funding-payer exits retained',fontsize=11,color=MUTED)
    ax = fig.add_axes([.10,.15,.82,.59]);style_axis(ax)
    x = [row['deviation_bps'] for row in points];y = [row['net_pnl_usdt'] for row in points]
    ax.plot(x,y,color=GREEN,lw=2,marker='o',markersize=5)
    ax.set_xlabel('Entry distance from equilibrium · bp',fontsize=13,labelpad=15)
    ax.set_ylabel('Net PnL · USDT',fontsize=13,labelpad=15)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v,_:f'{v:,.2f}'))
    ax.xaxis.set_major_locator(MultipleLocator(.5 if x[-1]-x[0]<=5 else 1))
    ax.margins(x=.03,y=.2)
    fig.savefig(output,facecolor=SURFACE);plt.close(fig)


def replay_day(day, manifest, deviations, output, extracted, exit_on_opposite_signal=False):
    """Run fresh independent engines on one day of the shared sorted sources."""
    start = int(datetime.strptime(day,'%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp()*1000)
    end = start+DAY
    output.mkdir(parents=True,exist_ok=True)
    sources = output/'sources.tsv'
    with sources.open('w') as handle:
        for kind,binary,_ in extracted:handle.write(f'{kind} {start} {end} {binary} BINARY\n')
    small = output/'funding-premium.csv'
    with small.open('w',newline='') as handle:
        writer=csv.writer(handle);writer.writerow(COLUMNS);writer.writerows(small_events(manifest,start,end))
    points = [];coverage = None
    for deviation in deviations:
        directory=output/'runs'/f'{deviation:.2f}bp'
        command=[str(MODEL/'build/archive-replay'),str(sources),str(small),str(directory),
                 '--equilibrium-bps','-5','--entry-offset-bps',str(deviation),
                 '--perp-maker-bps','0.5','--perp-taker-bps','2','--spot-taker-bps','2',
                 '--one-order-per-entry-event','1','--single-basis-position','1',
                 '--exit-on-opposite-signal',str(int(exit_on_opposite_signal))]
        subprocess.run(command,check=True,capture_output=True,text=True)
        summary=json.loads((directory/'summary.json').read_text())
        validate_run(directory,summary,manifest,start,end,deviation,exit_on_opposite_signal)
        current=(directory/'coverage.csv').read_bytes()
        if coverage is not None and current!=coverage:raise ValueError('Sweep used different market observations')
        coverage=current
        points.append(dict(deviation_bps=deviation,long_entry_basis_bps=-5-deviation,
                           short_entry_basis_bps=-5+deviation,net_pnl_usdt=summary['net_pnl'],
                           funding_pnl_usdt=summary['funding_pnl'],price_pnl_after_fees_usdt=summary['price_pnl']-summary['fees'],
                           fees_usdt=summary['fees'],long_net_pnl_usdt=summary['basis_sides'][SIDES[0]]['net_pnl'],
                           short_net_pnl_usdt=summary['basis_sides'][SIDES[1]]['net_pnl'],
                           partial_pair_fills=summary['paired_partial_fills'],closed_pairs=summary['closed_pairs'],
                           opposite_signal_exit_pairs=summary['opposite_signal_exit_pairs'],funding_exit_pairs=summary['funding_exit_pairs'],
                           final_signed_position_units=summary['perp_btc']/.01,
                           max_position_units=summary['max_open_gross_perp_btc']/.01))
        print(f'{day} · {deviation:.2f} bp: {summary["net_pnl"]:+.6f} USDT',flush=True)
    # Paths into temporary sorted sources are only useful during replay.
    sources.unlink()
    with (output/'sweep.csv').open('w',newline='') as handle:
        writer=csv.DictWriter(handle,fieldnames=list(points[0]));writer.writeheader();writer.writerows(points)
    (output/'sweep.json').write_text(json.dumps(dict(day=day,points=points),indent=2)+'\n')
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--day',help='UTC YYYY-MM-DD; default: random available archived day')
    parser.add_argument('--seed',type=int,help='Random selection seed; default: generated and recorded')
    parser.add_argument('--start-bps',default='1')
    parser.add_argument('--stop-bps',default='5')
    parser.add_argument('--step-bps',default='0.25')
    parser.add_argument('--exit-on-opposite-signal',action='store_true')
    parser.add_argument('--output',type=Path,default=MODEL/'results/entry-deviation-sweep')
    args = parser.parse_args()
    deviations = deviation_grid(args.start_bps,args.stop_bps,args.step_bps)
    if deviations[-1]>=9995:parser.error('Entry distance would produce an invalid basis threshold')
    candidates = available_days();seed = args.seed if args.seed is not None else secrets.randbits(64)
    day = args.day or random.Random(seed).choice(sorted(candidates))
    if day not in candidates:parser.error('Selected day has no complete archived inputs')
    manifest = candidates[day]
    start = int(datetime.strptime(day,'%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp()*1000);end=start+DAY
    args.output = args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    if (args.output/'selection.json').exists():
        parser.error('Output already contains a sweep; choose a new --output directory')
    selection = dict(day=day,seed=seed,selection='explicit day' if args.day else 'uniform random archived UTC day',
                     candidates=sorted(candidates),start_ms=start,end_ms=end,deviations_bps=deviations)
    (args.output/'selection.json').write_text(json.dumps(selection,indent=2)+'\n')
    print(f'Selected {day} UTC from {len(candidates)} days; seed={seed}; {len(deviations)} thresholds',flush=True)
    for name,source in manifest['sources'].items():
        if sha256(ROOT/source['path'])!=source['sha256']:raise ValueError(f'{name}: source checksum mismatch')
    print('Source archive checksums verified',flush=True)
    subprocess.run(['make','-C',str(MODEL),'build/archive-replay','build/archive-sort'],check=True)
    points = [];coverage = None
    with tempfile.TemporaryDirectory(prefix='deviation-sweep-',dir=MODEL/'build') as temporary:
        folder = Path(temporary)
        sources = [('Q','book'),('T','trades'),('S','spot_trades')]
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(extract_day,kind,name,manifest,folder,start,end) for kind,name in sources]
            extracted = [future.result() for future in futures]
        with (folder/'sources.tsv').open('w') as handle:
            for kind,binary,_ in extracted:handle.write(f'{kind} {start} {end} {binary} BINARY\n')
        small = args.output/'funding-premium.csv'
        with small.open('w',newline='') as handle:
            writer=csv.writer(handle);writer.writerow(COLUMNS);writer.writerows(small_events(manifest,start,end))
        provenance = dict(**selection,sources=manifest,sorted_day_sha256={kind:digest for kind,_,digest in extracted},
                          funding_premium_sha256=sha256(small),engine_sha256=sha256(MODEL/'build/archive-replay'),
                          archive_sort_sha256=sha256(MODEL/'build/archive-sort'),
                          script_sha256=sha256(Path(__file__).resolve()),
                          position_limit_trades=[-1,1],trade_btc=.01,equilibrium_bps=-5,
                          fees_bps=dict(perp_maker=.5,perp_taker=2,spot_taker=2),
                          exit_on_opposite_signal=args.exit_on_opposite_signal,
                          pnl_definition='Final marked equity including funding and executed fees; flat start; no terminal liquidation',
                          exits=('Opposite entry band closes to flat at taker prices and consumes that episode; ' if args.exit_on_opposite_signal else '')+
                                'anticipated funding payers close in the last 60 seconds before funding; causal premium forecast',
                          interpretation='One-day sensitivity analysis; highest observed PnL is not an out-of-sample optimum')
        (args.output/'inputs.json').write_text(json.dumps(provenance,indent=2)+'\n')
        points = replay_day(day,manifest,deviations,args.output,extracted,args.exit_on_opposite_signal)
    render(points,day,args.output/'pnl-vs-deviation.png',args.exit_on_opposite_signal)
    print(f'Saved {args.output/"pnl-vs-deviation.png"}',flush=True)


if __name__=='__main__':main()
