#!/usr/bin/env python3
"""Replay separate UTC days and test mean 24-hour marked PnL against zero.

Requires scipy and matplotlib. Uses existing, checksum-verified daily archives;
does not download data or change strategy parameters. Temporary tick CSVs are
deleted after each trial. Daily resets are an experimental horizon, not exits.
"""
import argparse
import hashlib
import json
import math
import os
import subprocess
import tempfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta, timezone
from pathlib import Path

from download_binance_maker import ROOT, prepare

MODEL = ROOT / 'Primary-Market-Model'
SETTINGS = ['--equilibrium-bps', '-5', '--max-gross-perp-btc', '0',
            '--exit-on-equilibrium', '0', '--exit-before-funding', '1',
            '--funding-cashflows', '1', '--funding-exit-payers-only', '1']


def digest(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def run_day(day, premium, work, binary_hash, premium_hash, settings, retain_events=None):
    manifest = ROOT / 'data/binance/maker' / day / 'events-spot-trades-full-day.manifest.json'
    metadata = json.loads(manifest.read_text())
    start = int(datetime.strptime(day, '%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp()*1000)
    if metadata['start_ms'] != start or metadata['end_ms'] != start+86400000:
        raise ValueError(f'{day}: expected a complete UTC day')
    paths = {}
    hashes = {}
    for name, source in metadata['sources'].items():
        path = ROOT / source['path']
        actual = digest(path)
        if actual != source['sha256']:
            raise ValueError(f'{day}: checksum mismatch for {path}')
        paths[name] = path
        hashes[name] = actual
    if set(paths) != {'book', 'trades', 'spot_trades', 'mark', 'funding'}:
        raise ValueError(f'{day}: need actual spot-trade reference and all perp feeds')
    signature = dict(binary_sha256=binary_hash, premium_sha256=premium_hash,
                     archives=hashes, settings=settings, start_ms=start, end_ms=start+86400000)
    saved = Path(work) / f'{day}.json'
    if saved.exists():
        result = json.loads(saved.read_text())
        if result.get('signature') == signature and retain_events is None:
            return result
    with tempfile.TemporaryDirectory(prefix=f'basis-trial-{day}-') as folder:
        directory = Path(folder)
        events = Path(retain_events)/f'{day}.csv' if retain_events else directory/'events.csv'
        counts = prepare(paths, start, start+86400000, events)
        if counts != metadata['counts']:
            raise ValueError(f'{day}: regenerated event counts differ from manifest')
        subprocess.run([str(MODEL/'build/basis-maker'), '--events', str(events),
                        '--premium-events', str(premium), *settings,
                        '--output', str(directory/'replay')], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        summary = json.loads((directory/'replay/summary.json').read_text())
    for actual, expected in [(summary['price_pnl']+summary['funding_pnl']-summary['fees'], summary['net_pnl']),
                             (summary['realized_pnl']+summary['unrealized_pnl'], summary['net_pnl'])]:
        if not math.isclose(actual, expected, abs_tol=1e-5):
            raise ValueError(f'{day}: accounting does not reconcile')
    options = dict(zip(settings[::2],settings[1::2]))
    for field in ('perp_maker_bps','perp_taker_bps','spot_taker_bps','entry_offset_bps'):
        if summary[field] != float(options['--'+field.replace('_','-')]):
            raise ValueError(f'{day}: incorrect fee setting {field}')
    expected_fees = sum(summary[leg+'_notional']*summary[leg+'_bps']/10000
                        for leg in ('perp_maker','perp_taker','spot_taker'))
    if not math.isclose(summary['fees'],expected_fees,rel_tol=1e-9,abs_tol=1e-5):
        raise ValueError(f'{day}: fees do not reconcile to executed leg notionals')
    if summary['entry_band_violations'] != 0 or summary['convergence_exit_pairs'] != 0 or abs(summary['delta_btc']) > 1e-8:
        raise ValueError(f'{day}: strategy settings or equal-BTC hedge failed')
    result = dict(date=day, signature=signature, event_counts=counts, summary=summary)
    saved.write_text(json.dumps(result, indent=2)+'\n')
    return result


def analyze(values, bootstrap_samples=20000, seed=20261018):
    """Preselected one-sided H0: mean <= 0; H1: mean > 0.

    HAC uses Bartlett weights, n/(n-1) correction and an asymptotic normal
    reference, not an exact t law. Bootstrap resamples circular consecutive
    blocks, preserves within-block order and centers the series under H0.
    """
    import numpy as np
    from scipy import stats
    x = np.asarray(values, dtype=float)
    if x.ndim != 1 or len(x) < 3 or not np.isfinite(x).all():
        raise ValueError('Need at least three finite trial PnLs')
    n = len(x)
    if np.std(x, ddof=1) == 0:
        raise ValueError('Zero variance: a t-test is undefined')
    mean = float(x.mean())
    se = float(stats.sem(x))
    test = stats.ttest_1samp(x, 0, alternative='greater')
    result = dict(n=n, total=float(x.sum()), mean=mean, median=float(np.median(x)),
                  std=float(x.std(ddof=1)), positive=int((x > 0).sum()),
                  minimum=float(x.min()), maximum=float(x.max()),
                  lag1_correlation=float(np.corrcoef(x[:-1], x[1:])[0, 1]),
                  t_test=dict(statistic=float(test.statistic), df=n-1,
                              p_one_sided=float(test.pvalue), standard_error=se,
                              ci95_two_sided=[mean-float(stats.t.ppf(.975,n-1))*se,
                                              mean+float(stats.t.ppf(.975,n-1))*se]),
                  hac={}, block_bootstrap={})
    residual = x-mean
    for requested_lag in (3, 7):
        lag = min(requested_lag, n-1)
        meat = float(residual @ residual)
        for k in range(1, lag+1):
            meat += 2*(1-k/(lag+1))*float(residual[k:] @ residual[:-k])
        variance = meat/n**2*n/(n-1)
        if variance <= 0:
            raise ValueError('Nonpositive HAC variance')
        robust_se = math.sqrt(variance)
        z = mean/robust_se
        result['hac'][str(requested_lag)] = dict(effective_lags=lag, standard_error=robust_se, statistic=z,
            p_one_sided=float(stats.norm.sf(z)),
            ci95_asymptotic=[mean-1.959963984540054*robust_se, mean+1.959963984540054*robust_se])
    for length in (3, 7):
        rng = np.random.default_rng(seed+length)
        starts = rng.integers(0, n, size=(bootstrap_samples, math.ceil(n/length)))
        indices = ((starts[..., None]+np.arange(length)) % n).reshape(bootstrap_samples, -1)[:, :n]
        boot = x[indices].mean(axis=1)
        # Positive sample mean compared with bootstrap means under the null.
        p = float((1+np.count_nonzero(boot-mean >= mean))/(bootstrap_samples+1))
        quantiles = np.quantile(boot-mean, [.025, .975])
        result['block_bootstrap'][str(length)] = dict(samples=bootstrap_samples,
            seed=seed+length, p_one_sided_centered=p,
            ci95_basic=[mean-float(quantiles[1]), mean-float(quantiles[0])])
    return result


def render(report, output):
    os.environ.setdefault('MPLCONFIGDIR', str(MODEL/'build/matplotlib-cache'))
    os.environ.setdefault('XDG_CACHE_HOME', str(MODEL/'build/cache'))
    from presentation import render_trials
    return render_trials(report, output)


def make_report(trials):
    dates = [datetime.strptime(r['date'], '%Y-%m-%d') for r in trials]
    if len(dates) < 3 or any((b-a).days != 1 for a,b in zip(dates, dates[1:])):
        raise ValueError('Trials must be distinct, consecutive, chronological UTC days')
    return dict(trial_definition='Distinct nonoverlapping UTC days; each starts flat, fixed 0.01 BTC quotes; end marked at mids without liquidation',
        hypothesis='H0: expected 24h marked PnL <= 0; H1: expected 24h marked PnL > 0',
        interpretation='In-sample simulated PnL after configured execution fees, not proof of live or out-of-sample profitability. Daily resets differ from continuously held monthly positions.',
        limitations=['Only one month already used for strategy development; observations may not be independent or stationary.',
                     'Configured maker/taker fees applied to executed fills; borrowing costs, market impact, depth, margin and liquidation omitted. Unlimited capital and spot borrowing assumed.',
                     'Funding signal uses causal minute-close premium proxies, not exact historical funding forecasts.',
                     'Daily terminal PnL includes unrealized marks and omits liquidation spreads; USD PnL is not a return on fixed capital.',
                     'Resampling reuses observed days and creates no new market history. HAC and bootstrap are small-sample diagnostics.'],
        methods=dict(primary='One-sided one-sample t-test of daily marked PnL against zero, alpha 0.05; 95% two-sided t interval',
                     robustness='Bartlett/Newey-West HAC at 3 and 7 daily lags (asymptotic normal reference); centered circular block bootstrap at block lengths 3 and 7, 20,000 resamples',
                     decomposition='Price-only statistics describe the same trade path after execution fees with funding cash removed; not a separate optimized strategy.',
                     sources=['https://www.itl.nist.gov/div898/handbook/prc/section2/prc.htm',
                              'https://www.statsmodels.org/stable/generated/statsmodels.stats.sandwich_covariance.cov_hac.html']),
        statistics=analyze([r['summary']['net_pnl'] for r in trials]),
        price_only_statistics=analyze([r['summary']['price_pnl']-r['summary']['fees'] for r in trials]), trials=trials)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--start',help='Explicit UTC start date for separate flat-start experiments')
    parser.add_argument('--end',help='Explicit UTC end date for separate flat-start experiments')
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--output', type=Path, default=MODEL/'results/trials.json')
    parser.add_argument('--from-results', type=Path)
    parser.add_argument('--perp-maker-bps',type=float,help='Maker bps per fill; default 0')
    parser.add_argument('--perp-taker-bps',type=float,help='Perp taker bps per fill; default 0.5')
    parser.add_argument('--spot-taker-bps',type=float,help='Spot taker bps per fill; default 0.5')
    parser.add_argument('--entry-offset-bps',type=float,help='Offset around -5 bps equilibrium; default 1.5')
    parser.add_argument('--continuous-output',type=Path,help='Also replay all dates continuously; temporarily retains normalized ticks until finished')
    args = parser.parse_args()
    if args.from_results:
        if args.continuous_output or any(v is not None for v in (args.perp_maker_bps,args.perp_taker_bps,args.spot_taker_bps,args.entry_offset_bps)):
            parser.error('--from-results only analyzes saved fills; fee changes require a replay, as do entry-rule changes')
        saved = json.loads(args.from_results.read_text())
        if saved.get('continuous_inventory'):
            report = dict(saved)
            report['statistics'] = analyze([r['summary']['net_pnl'] for r in saved['trials']])
            report['price_only_statistics'] = analyze([r['summary']['price_pnl']-r['summary']['fees'] for r in saved['trials']])
        else:
            report = make_report(saved['trials'])
    else:
        if not args.start or not args.end:
            parser.error('Specify --start and --end for flat-start experiments, or --from-results to redraw the current continuous analysis')
        args.perp_maker_bps = 0 if args.perp_maker_bps is None else args.perp_maker_bps
        args.perp_taker_bps = .5 if args.perp_taker_bps is None else args.perp_taker_bps
        args.spot_taker_bps = .5 if args.spot_taker_bps is None else args.spot_taker_bps
        args.entry_offset_bps = 1.5 if args.entry_offset_bps is None else args.entry_offset_bps
        if not 1 <= args.workers <= 4:
            parser.error('workers must be between 1 and 4')
        start = datetime.strptime(args.start,'%Y-%m-%d')
        end = datetime.strptime(args.end,'%Y-%m-%d')
        if end < start or (end-start).days < 2:
            parser.error('Need at least three distinct UTC days')
        days = [(start+timedelta(days=i)).strftime('%Y-%m-%d') for i in range((end-start).days+1)]
        if args.continuous_output and args.start[:7] != args.end[:7]:
            parser.error('--continuous-output currently requires dates in one calendar month')
        if any(not math.isfinite(v) or not 0 <= v < 10000 for v in (args.perp_maker_bps,args.perp_taker_bps,args.spot_taker_bps)):
            parser.error('Fee rates must be finite and in [0,10000) bps')
        if not math.isfinite(args.entry_offset_bps) or not 0 <= args.entry_offset_bps < 9995:
            parser.error('Entry offset must be finite and in [0,9995) bps')
        settings = [*SETTINGS,'--perp-maker-bps',str(args.perp_maker_bps),
                    '--perp-taker-bps',str(args.perp_taker_bps),'--spot-taker-bps',str(args.spot_taker_bps),
                    '--entry-offset-bps',str(args.entry_offset_bps)]
        subprocess.run(['make','-C',str(MODEL),'build/basis-maker'],check=True)
        binary_hash = digest(MODEL/'build/basis-maker')
        setting_hash = hashlib.sha256(json.dumps(settings).encode()).hexdigest()[:12]
        work = MODEL/'build'/f'trials-{args.start}_{args.end}'/setting_hash
        work.mkdir(parents=True,exist_ok=True)
        results = {}
        with tempfile.TemporaryDirectory(prefix='basis-trials-input-') as retained:
            with ProcessPoolExecutor(max_workers=args.workers) as pool:
                pending = {}
                for day in days:
                    premium = ROOT/'data/binance/maker'/day[:7]/'premium-events.csv'
                    pending[pool.submit(run_day, day, premium, work, binary_hash, digest(premium),
                                        settings, retained if args.continuous_output else None)] = day
                for future in as_completed(pending):
                    day = pending[future]
                    results[day] = future.result()
                    print(f"Completed {day}: {len(results)}/{len(days)}; net {results[day]['summary']['net_pnl']:+,.2f} USDT",flush=True)
            if args.continuous_output:
                event_list = Path(retained)/'events-list.txt'
                event_list.write_text(''.join(str(Path(retained)/f'{day}.csv')+'\n' for day in days))
                premium = ROOT/'data/binance/maker'/args.start[:7]/'premium-events.csv'
                print('Running continuous replay with the same fee settings...',flush=True)
                subprocess.run([str(MODEL/'build/basis-maker'),'--events-list',str(event_list),
                                '--premium-events',str(premium),*settings,'--output',str(args.continuous_output)],check=True)
        trials = [results[day] for day in days]
        report = make_report(trials)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    render(report,args.output.with_suffix('.png'))
    print(json.dumps(report['statistics'],indent=2),flush=True)
    print(f'Results: {args.output}\nGraph: {args.output.with_suffix(".png")}',flush=True)


if __name__ == '__main__':
    main()
