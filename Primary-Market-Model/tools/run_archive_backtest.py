#!/usr/bin/env python3
"""Continuously replay monthly source ZIPs without retaining expanded tick CSVs."""
import argparse
import bisect
import csv
import hashlib
import json
import math
import os
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from download_binance_maker import ROOT, COLUMNS, event, rows, millis
from run_backtest import MODEL, render_report, render_png
from run_trials import analyze, render as render_trials


def months_between(start, end):
    first = datetime.strptime(start, '%Y-%m')
    last = datetime.strptime(end, '%Y-%m')
    if last < first: raise ValueError('End month precedes start month')
    result = []
    while first <= last:
        result.append(first.strftime('%Y-%m'))
        first = first.replace(year=first.year+(first.month==12),month=first.month%12+1)
    return result


def continuous_statistics(directory, summary):
    daily = {}
    with (directory/'equity.csv').open() as handle:
        for row in csv.DictReader(handle): daily[int(row['time_ms'])//86400000] = row
    keys = sorted(daily)
    if any(b!=a+1 for a,b in zip(keys,keys[1:])): raise ValueError('Missing UTC equity day')
    previous = dict(net_pnl=0,price_pnl=0,fees=0,funding_pnl=0)
    trials = []
    for day in keys:
        cumulative = {key:float(daily[day][key]) for key in previous}
        increment = {key:cumulative[key]-previous[key] for key in previous}
        previous = cumulative
        for key in ('perp_maker_bps','perp_taker_bps','spot_taker_bps','equilibrium_bps','entry_bid_basis_bps','entry_ask_basis_bps'):
            increment[key] = summary[key]
        increment['funding_exit_flat_region_exempt']=summary.get('funding_exit_flat_region_exempt',True)
        increment['one_order_per_entry_event']=summary.get('one_order_per_entry_event',False)
        increment['order_btc']=summary.get('order_btc',.01)
        increment['single_basis_position']=summary.get('single_basis_position',False)
        increment['exit_on_opposite_signal']=summary.get('exit_on_opposite_signal',False)
        trials.append(dict(date=datetime.fromtimestamp(day*86400,timezone.utc).strftime('%Y-%m-%d'),summary=increment))
    if not math.isclose(sum(t['summary']['net_pnl'] for t in trials),summary['net_pnl'],abs_tol=1e-6):
        raise ValueError('Daily equity changes do not reconcile')
    return dict(continuous_inventory=True,primary_inference='hac_7',
        methods=dict(primary='One-sided mean test with Bartlett/Newey-West HAC standard error at 7 daily lags; asymptotic normal reference and 95% two-sided interval',
                     robustness='HAC at 3 daily lags; centered circular block bootstrap with 3- and 7-day blocks, 20,000 resamples',
                     descriptive_only='Ordinary one-sample t-test retained for comparison; its independence assumption is not established'),
        trial_definition='UTC daily changes in total marked equity from one continuous run; inventory carries across days and months',
        hypothesis='H0: mean daily marked equity change <= 0; H1: mean > 0',
        interpretation='Hypothesis tested on previously inspected history; dependent daily USD PnL, not fixed-capital returns or proof of live profitability',
        sample_note='Hypothesis tested on previously inspected history. Carried inventory can induce daily dependence; these are in-sample diagnostics, with 7-lag HAC selected before inspecting the new results.',
        limitations=['Queue, impact, depth, borrowing costs, margin and liquidation omitted.'+
                     (' Position limited to one signed trade.' if summary.get('single_basis_position') else ' Unlimited inventory.'),
                     'Spot last-trade midpoint proxy and assumed 1 bp full spread; zero latency.',
                     'Funding exits use causal premium-minute CLOSE proxies.',
                     'HAC and block bootstrap address some serial dependence but not structural change or fill-model error.'],
        statistics=analyze([t['summary']['net_pnl'] for t in trials]),
        price_only_statistics=analyze([t['summary']['price_pnl']-t['summary']['fees'] for t in trials]),trials=trials)


def verify_funding(directory, manifests, start_ms=None, end_ms=None):
    """Independently reconcile each payment using held BTC and the source rate.

    Exact pre/post-settlement marks retain inventory after any exits that occur
    later than the first sampled mark in the pre-funding minute.
    """
    with (directory/'equity.csv').open() as handle: points=list(csv.DictReader(handle))
    times=[int(point['time_ms']) for point in points]
    count=0;maximum_error=0;paid=0;received=0;paying=[]
    for manifest in manifests:
        marks={millis(row[0]):float(row[1]) for row in rows(ROOT/manifest['sources']['mark']['path'])}
        for row in rows(ROOT/manifest['sources']['funding']['path']):
            timestamp=millis(row[0])
            if start_ms is not None and timestamp<start_ms:continue
            if end_ms is not None and timestamp>=end_ms:continue
            index=bisect.bisect_left(times,timestamp)
            if index==len(points):raise ValueError('No equity observation after funding settlement')
            previous=points[index-1] if index else {}
            expected=-float(previous.get('perp_btc',0))*marks[timestamp//60000*60000]*float(row[2])
            actual=float(points[index]['funding_pnl'])-float(previous.get('funding_pnl',0))
            maximum_error=max(maximum_error,abs(actual-expected))
            if not math.isclose(actual,expected,rel_tol=1e-8,abs_tol=.01):
                raise ValueError(f'Funding reconciliation failed at {timestamp}: {actual} versus {expected}')
            for side,sign in (('long_perp_short_spot',1),('short_perp_long_spot',-1)):
                key=side+'_funding_pnl'
                side_expected=-sign*float(previous.get(side+'_open_btc',0))*marks[timestamp//60000*60000]*float(row[2])
                side_actual=float(points[index][key])-float(previous.get(key,0))
                if not math.isclose(side_actual,side_expected,rel_tol=1e-8,abs_tol=.01):
                    raise ValueError(f'{side} funding reconciliation failed at {timestamp}')
                paid+=max(0,-side_actual);received+=max(0,side_actual)
                if side_actual<-.000001:
                    paying.append(dict(time_ms=timestamp,side=side,paid_usdt=-side_actual))
            count+=1
    return dict(funding_settlements=count,max_payment_error_usdt=maximum_error,
                gross_funding_paid_usdt=paid,gross_funding_received_usdt=received,
                paying_side_settlements=paying)


def verify_sides(directory, summary):
    """Price, funding and fees must each reconcile independently at every mark."""
    sides=('long_perp_short_spot','short_perp_long_spot')
    checks={'unrealized_pnl':lambda r:float(r['unrealized_pnl']),
            'realized_price_pnl':lambda r:float(r['realized_pnl'])-float(r['funding_pnl']),
            'funding_pnl':lambda r:float(r['funding_pnl']),
            'fees':lambda r:float(r['fees']), 'net_pnl':lambda r:float(r['net_pnl']),
            'open_btc':lambda r:float(r['open_gross_perp_btc'])}
    errors={key:0 for key in checks}; count=0
    with (directory/'equity.csv').open() as handle:
        for row in csv.DictReader(handle):
            for key,expected in checks.items():
                actual=sum(float(row[side+'_'+key]) for side in sides)
                error=abs(actual-expected(row));errors[key]=max(errors[key],error)
                if not math.isfinite(actual) or error>(1e-6 if key=='open_btc' else .05):
                    raise ValueError(f'Direction {key} attribution failed at {row["time_ms"]}: {error}')
            count+=1
    for side in sides:
        for key in checks:
            if not math.isclose(float(row[side+'_'+key]),summary['basis_sides'][side][key],abs_tol=1e-6):
                raise ValueError(f'Final {side} {key} does not match its summary')
    return dict(side_attribution_marks=count,max_side_attribution_errors=errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--start-month', default='2023-12')
    parser.add_argument('--end-month', default='2024-02')
    parser.add_argument('--output', type=Path, default=MODEL/'build/current')
    parser.add_argument('--report-dir',type=Path,default=MODEL/'results',help='Keep experiment plots and statistics separate from the baseline')
    parser.add_argument('--equilibrium-bps',type=float,default=-5,help='Configured midpoint-basis target; defaults to the original -5 bp hypothesis')
    parser.add_argument('--entry-offset-bps',type=float,default=1.25)
    parser.add_argument('--perp-maker-bps',type=float,default=.5)
    parser.add_argument('--perp-taker-bps',type=float,default=2)
    parser.add_argument('--spot-taker-bps',type=float,default=2)
    parser.add_argument('--one-order-per-entry-event',type=int,choices=(0,1),default=1)
    parser.add_argument('--single-basis-position',type=int,choices=(0,1),default=1)
    parser.add_argument('--exit-on-opposite-signal',type=int,choices=(0,1),default=1)
    parser.add_argument('--daily-reset',type=int,choices=(0,1),default=0)
    parser.add_argument('--close-at-day-end',type=int,choices=(0,1),default=0)
    parser.add_argument('--wait-for-inputs', action='store_true',help='Allow verified monthly downloads to finish while earlier months replay')
    parser.add_argument('--stream-source', nargs=3,help=argparse.SUPPRESS)
    args = parser.parse_args()
    if (not math.isfinite(args.equilibrium_bps) or not math.isfinite(args.entry_offset_bps)
            or args.entry_offset_bps<0 or args.equilibrium_bps-args.entry_offset_bps<=-10000):
        parser.error('Invalid basis target or entry offset')
    if any(not math.isfinite(value) or value<0 or value>=10000 for value in
           (args.perp_maker_bps,args.perp_taker_bps,args.spot_taker_bps)):
        parser.error('Fees must be finite and in [0,10000) bps')
    if args.stream_source:
        month,key,work=args.stream_source
        deadline=time.monotonic()+7200
        while True:
            manifest=ROOT/'data/binance/maker'/month/'month-inputs.manifest.json'
            m=json.loads(manifest.read_text())
            if key in m['sources']:
                source=m['sources'][key];path=ROOT/source['path']
                with path.open('rb') as handle: digest=hashlib.file_digest(handle,'sha256').hexdigest()
                if digest!=source['sha256']:raise ValueError('Stream source checksum mismatch')
                kind={'book':'Q','trades':'T','spot_trades':'S'}[key]
                unzip=subprocess.Popen(['unzip','-p',str(path)],stdout=subprocess.PIPE)
                try:
                    subprocess.run([str(MODEL/'build/archive-sort'),kind,str(Path(work)/(month+'-'+key)),work],stdin=unzip.stdout,check=True)
                    unzip.stdout.close()
                    if unzip.wait()!=0:raise RuntimeError('Archive decompression failed')
                finally:
                    if not unzip.stdout.closed:unzip.stdout.close()
                    if unzip.poll() is None:unzip.terminate()
                    unzip.wait()
                return
            if time.monotonic()>deadline:raise TimeoutError('Source download did not finish within two hours')
            time.sleep(2)
    months = months_between(args.start_month,args.end_month)
    manifests = []
    for month in months:
        path = ROOT/'data/binance/maker'/month/'month-inputs.manifest.json'
        m = json.loads(path.read_text())
        required={'mark','funding','premium'} if args.wait_for_inputs else {'book','trades','spot_trades','mark','funding','premium'}
        if not required.issubset(m['sources']) or (not args.wait_for_inputs and m['status']!='checksum_verified'):
            raise ValueError(f'{month}: incomplete archive manifest')
        for name,source in m['sources'].items():
            file = ROOT/source['path']
            with file.open('rb') as handle: actual = hashlib.file_digest(handle,'sha256').hexdigest()
            if actual != source['sha256']: raise ValueError(f'{month} {name}: checksum mismatch')
        manifests.append(m)
        print(f'Available input hashes verified: {month}',flush=True)
    subprocess.run(['make','-C',str(MODEL),'build/archive-replay','build/archive-sort'],check=True)
    args.output.mkdir(parents=True,exist_ok=True)
    # Seed the known eight-hour schedule before the first UTC midnight. This
    # cancels entries during any millisecond delay in its recorded settlement;
    # it never uses the upcoming funding rate as an exit signal or cash flow.
    small = [event(manifests[0]['start_ms']-8*3600000,'R',rate=0,interval=8)]
    for m in manifests:
        marks = {millis(row[0]):row[1] for row in rows(ROOT/m['sources']['mark']['path'])}
        for row in rows(ROOT/m['sources']['funding']['path']):
            t = millis(row[0])
            small.append(event(t,'F',marks[t//60000*60000],rate=row[2],interval=row[1]))
        for row in rows(ROOT/m['sources']['premium']['path']): small.append(event(millis(row[6])+1,'P',row[4]))
    small.sort(key=lambda row:(int(row[0]),0 if row[1]=='F' else 1))
    streams = []
    with tempfile.TemporaryDirectory(prefix='archive-replay-',dir=MODEL/'build') as temporary:
        folder = Path(temporary)
        with (folder/'small.csv').open('w',newline='') as handle:
            writer=csv.writer(handle); writer.writerow(COLUMNS); writer.writerows(small)
        try:
            with (folder/'sources.tsv').open('w') as handle:
                for m in manifests:
                    for kind,name in [('Q','book'),('T','trades'),('S','spot_trades')]:
                        process=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--stream-source',m['month'],name,str(folder)],stdout=subprocess.PIPE,start_new_session=True)
                        streams.append(process)
                        handle.write(f'{kind} {m["start_ms"]} {m["end_ms"]} /dev/fd/{process.stdout.fileno()} BINARY\n')
            subprocess.run([str(MODEL/'build/archive-replay'),str(folder/'sources.tsv'),str(folder/'small.csv'),
                            str(args.output),'--equilibrium-bps',str(args.equilibrium_bps),
                            '--entry-offset-bps',str(args.entry_offset_bps),
                            '--perp-maker-bps',str(args.perp_maker_bps),
                            '--perp-taker-bps',str(args.perp_taker_bps),
                            '--spot-taker-bps',str(args.spot_taker_bps),
                            '--one-order-per-entry-event',str(args.one_order_per_entry_event),
                            '--single-basis-position',str(args.single_basis_position),
                            '--exit-on-opposite-signal',str(args.exit_on_opposite_signal),
                            '--daily-reset',str(args.daily_reset),
                            '--close-at-day-end',str(args.close_at_day_end)],check=True,
                           pass_fds=tuple(p.stdout.fileno() for p in streams))
            for process in streams:
                process.stdout.close()
                if process.wait()!=0: raise RuntimeError('Archive decompression failed')
        finally:
            for process in streams:
                if not process.stdout.closed: process.stdout.close()
                if process.poll() is None: os.killpg(process.pid,signal.SIGTERM)
                process.wait()
    manifests=[json.loads((ROOT/'data/binance/maker'/month/'month-inputs.manifest.json').read_text()) for month in months]
    if any(m['status']!='checksum_verified' for m in manifests):raise ValueError('Monthly download did not complete validation')
    if args.daily_reset:
        from daily_archive_trials import finalize_daily_trials
        return finalize_daily_trials(args,manifests)
    summary=json.loads((args.output/'summary.json').read_text())
    if summary['equilibrium_bps']!=args.equilibrium_bps or summary['entry_offset_bps']!=args.entry_offset_bps:
        raise ValueError('Replay used different hypothesis parameters than requested')
    for key in ('perp_maker_bps','perp_taker_bps','spot_taker_bps'):
        if summary[key]!=getattr(args,key):raise ValueError(f'Replay used different fees: {key}')
    if summary['one_order_per_entry_event']!=bool(args.one_order_per_entry_event):
        raise ValueError('Replay used a different entry-event rule')
    if args.one_order_per_entry_event and summary['max_entry_event_btc']>summary['order_btc']+1e-9:
        raise ValueError('A threshold event exceeded its single-order BTC budget')
    if summary['single_basis_position']!=bool(args.single_basis_position):
        raise ValueError('Replay used a different position-limit rule')
    if summary['exit_on_opposite_signal']!=bool(args.exit_on_opposite_signal):
        raise ValueError('Replay used a different opposite-signal exit rule')
    if args.single_basis_position and summary['max_open_gross_perp_btc']>summary['order_btc']+1e-9:
        raise ValueError('Single basis position exceeded its one-trade limit')
    with (args.output/'coverage.csv').open() as handle:
        quote_coverage=[row for row in csv.DictReader(handle) if row['kind']=='Q']
    summary['max_quote_gap_ms']=max(int(row['max_gap_ms']) for row in quote_coverage)
    summary['quote_gap_days']=[dict(date=datetime.fromtimestamp(int(row['utc_day'])*86400,timezone.utc).strftime('%Y-%m-%d'),
                                    max_gap_ms=int(row['max_gap_ms'])) for row in quote_coverage if int(row['max_gap_ms'])>10000]
    summary['final_spot_notional']=summary['spot_btc']*summary['final_spot_mid']
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    if summary['entry_band_violations'] or summary['convergence_exit_pairs'] or abs(summary['delta_btc'])>1e-8:
        raise ValueError('Strategy or hedge invariant failed')
    expected=sum(summary[leg+'_notional']*summary[leg+'_bps']/10000 for leg in ('perp_maker','perp_taker','spot_taker'))
    if not math.isclose(summary['fees'],expected,rel_tol=1e-9,abs_tol=1e-4): raise ValueError('Fee reconciliation failed')
    validation=verify_funding(args.output,manifests)
    validation.update(verify_sides(args.output,summary))
    if args.single_basis_position:
        with (args.output/'equity.csv').open() as handle:
            for row in csv.DictReader(handle):
                long_btc=float(row['long_perp_short_spot_open_btc'])
                short_btc=float(row['short_perp_long_spot_open_btc'])
                if min(long_btc,short_btc)>1e-9:raise ValueError('Opposite basis positions held simultaneously')
        validation['single_basis_position_verified']=True
        validation['max_position_units']=summary['max_open_gross_perp_btc']/summary['order_btc']
    summary['funding_paid_usdt']=validation['gross_funding_paid_usdt']
    summary['funding_received_usdt']=validation['gross_funding_received_usdt']
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    (args.output/'validation.json').write_text(json.dumps(validation,indent=2)+'\n')
    provenance=dict(months=months,start_ms=manifests[0]['start_ms'],end_ms=manifests[-1]['end_ms'],
                    hypothesis=dict(equilibrium_bps=args.equilibrium_bps,entry_offset_bps=args.entry_offset_bps,
                                    one_order_per_entry_event=bool(args.one_order_per_entry_event),
                                    exit_on_opposite_signal=bool(args.exit_on_opposite_signal),
                                    single_basis_position=bool(args.single_basis_position)),
                    fees=dict(perp_maker_bps=args.perp_maker_bps,perp_taker_bps=args.perp_taker_bps,
                              spot_taker_bps=args.spot_taker_bps),
                    engine_sha256=hashlib.sha256((MODEL/'build/archive-replay').read_bytes()).hexdigest(),
                    archive_sort_sha256=hashlib.sha256((MODEL/'build/archive-sort').read_bytes()).hexdigest(),
                    sorting='Exact stable external sort; all source rows retained',sources=manifests)
    (args.output/'inputs.json').write_text(json.dumps(provenance,indent=2)+'\n')
    statistics=continuous_statistics(args.output,summary)
    statistics['data_quality']=dict(max_quote_gap_ms=summary['max_quote_gap_ms'],quote_gap_days=summary['quote_gap_days'])
    statistics['limitations'].append(f'Quote gaps reach {summary["max_quote_gap_ms"]/60000:.1f} minutes; stale books pause entries, while held positions remain marked at last observed prices.')
    args.report_dir.mkdir(parents=True,exist_ok=True)
    render_report(args.output,args.report_dir/'result.html')
    render_png(args.output,args.report_dir/'result.png')
    (args.report_dir/'trials.json').write_text(json.dumps(statistics,indent=2)+'\n')
    render_trials(statistics,args.report_dir/'trials.png')
    print(f'Continuous profit: {summary["net_pnl"]:+,.2f} USDT over {len(statistics["trials"])} days',flush=True)


if __name__=='__main__': main()
