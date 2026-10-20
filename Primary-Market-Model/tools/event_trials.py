"""Accounting and inference for completed continuous flat-to-flat positions."""
import csv
import hashlib
import json
import math
import os
import shutil
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

DAY = 86400000


def aggregate_events(rows, summary):
    """One observation per position, regardless of its number of partial fills."""
    groups = defaultdict(list)
    for row in rows:
        groups[int(row['position_id'])].append(row)
    events, incomplete = [], []
    total_fees = total_funding = open_pnl = 0.0
    for position_id, lots in sorted(groups.items()):
        if position_id <= 0 or len({r['direction'] for r in lots}) != 1:
            raise ValueError('Missing position ID or mixed directions within a position')
        direction = 1 if lots[0]['direction'] == 'LONG_PERP_SHORT_SPOT' else -1
        statuses = {r['status'] for r in lots}
        if len(statuses) != 1 or not statuses <= {'OPEN', 'CLOSED'}:
            raise ValueError('A position must be entirely open or entirely closed')
        btc = sum(float(r['perp_btc']) for r in lots)
        if btc > summary['order_btc'] + 1e-9:
            raise ValueError('Completed-position BTC exceeded one trade')
        fees = sum(float(r['entry_fees']) + float(r['exit_fees']) for r in lots)
        funding = sum(float(r['funding_pnl']) for r in lots)
        total_fees += fees
        total_funding += funding
        if statuses == {'OPEN'}:
            marked = sum(direction * float(r['perp_btc']) *
                ((summary['final_perp_mid'] - float(r['perp_price'])) -
                 (summary['final_spot_mid'] - float(r['spot_price']))) for r in lots) - fees + funding
            open_pnl += marked
            incomplete.append(dict(position_id=position_id, partial_fill_count=len(lots),
                                   btc=btc, marked_net_pnl=marked, funding_pnl=funding))
            continue
        exits = {int(r['exit_time_ms']) for r in lots}
        reasons = {r['exit_reason'] for r in lots}
        if len(exits) != 1 or len(reasons) != 1:
            raise ValueError('Partial fills must share their flat exit')
        price = sum(direction * float(r['perp_btc']) *
            ((float(r['exit_perp_price']) - float(r['perp_price'])) -
             (float(r['exit_spot_price']) - float(r['spot_price']))) for r in lots)
        net = price - fees + funding
        if not math.isclose(net, sum(float(r['realized_pnl']) for r in lots), abs_tol=1e-6):
            raise ValueError('Event lifetime PnL does not match lot accounting')
        entry, exit_time = min(int(r['time_ms']) for r in lots), next(iter(exits))
        if exit_time < entry:
            raise ValueError('Exit precedes entry')
        events.append(dict(position_id=position_id, entry_time_ms=entry, exit_time_ms=exit_time,
            direction=lots[0]['direction'], btc=btc, partial_fill_count=len(lots), price_pnl=price,
            fees=fees, funding_pnl=funding, net_pnl=net, exit_reason=next(iter(reasons)),
            duration_hours=(exit_time-entry)/3600000))
    closed_net = sum(e['net_pnl'] for e in events)
    for actual, expected in [(total_fees, summary['fees']), (total_funding, summary['funding_pnl']),
                             (closed_net + open_pnl, summary['net_pnl'])]:
        if not math.isclose(actual, expected, abs_tol=1e-5):
            raise ValueError(f'Event ledger fails portfolio reconciliation: {actual} versus {expected}')
    return events, incomplete


def event_statistics(events, start_ms, end_ms):
    import numpy as np
    from scipy import stats
    x = np.asarray([e['net_pnl'] for e in events])
    n = len(x)
    if n < 3 or not np.isfinite(x).all() or x.std(ddof=1) == 0:
        raise ValueError('Need at least three variable completed trade PnLs')
    mean, se = float(x.mean()), float(stats.sem(x))
    test = stats.ttest_1samp(x, 0, alternative='greater')
    critical = float(stats.t.ppf(.975, n-1))
    # Cluster scores retain all days, including days without completed trades.
    # This checks shared exit-day dependence. It cannot capture every dependence
    # channel in trades whose holding intervals span several days.
    first, last = start_ms//DAY, (end_ms-1)//DAY
    scores = np.zeros(last-first+1)
    for event in events:
        scores[event['exit_time_ms']//DAY-first] += event['net_pnl']-mean
    g = len(scores)
    meat = float(scores @ scores)
    cluster_se = math.sqrt(g/(g-1)*meat/n**2)
    hac_meat = meat
    for k in range(1, min(7, g-1)+1):
        hac_meat += 2*(1-k/8)*float(scores[k:] @ scores[:-k])
    hac_se = math.sqrt(g/(g-1)*max(0, hac_meat)/n**2)
    return dict(n=n, total=float(x.sum()), mean=mean, median=float(np.median(x)),
        positive=int((x>0).sum()), std=float(x.std(ddof=1)), minimum=float(x.min()), maximum=float(x.max()),
        lag1_correlation=float(np.corrcoef(x[:-1], x[1:])[0, 1]) if np.std(x[:-1])*np.std(x[1:])>0 else None,
        t_test=dict(statistic=float(test.statistic), df=n-1, p_one_sided=float(test.pvalue),
                    standard_error=se, ci95_two_sided=[mean-critical*se, mean+critical*se]),
        exit_day_cluster=dict(clusters=g, standard_error=cluster_se,
            p_one_sided=float(stats.t.sf(mean/cluster_se, g-1)) if cluster_se else None,
            note='All calendar exit-day clusters, including zero-event days; t reference with G-1 df.'),
        exit_day_hac_7=dict(days=g, standard_error=hac_se,
            p_one_sided=float(stats.norm.sf(mean/hac_se)) if hac_se else None,
            note='Bartlett weights through seven calendar-day lags on exit-day cluster scores; asymptotic normal reference.'))


def render_events(report, output):
    from presentation import GREEN, INK, MUTED, RED, SURFACE, compact, header, money_axis, settings_line, setup, style_axis
    import matplotlib.dates as mdates
    plt = setup()
    s, stats = report['summary'], report['statistics']
    events = report['events']
    times = [datetime.fromtimestamp(e['exit_time_ms']/1000, timezone.utc) for e in events]
    values = [e['net_pnl'] for e in events]
    fig = plt.figure(figsize=(16, 9), dpi=240, facecolor=SURFACE)
    header(fig, 'Performance per completed trade',
        f'Binance BTCUSDT   ·   {report["start_date"]} – {report["end_date"]} UTC   ·   {len(events):,} completed trades',
        settings_line(s),
        [(.07, 'MEAN TRADE PNL · USDT', compact(stats['mean'], True), INK),
         (.39, 'PROFITABLE TRADES', f'{stats["positive"]:,} / {stats["n"]:,}', GREEN),
         (.71, 'ONE-SIDED t-TEST p-VALUE', f'{stats["t_test"]["p_one_sided"]:.2g}', INK)])
    fig.text(.07, .813, 'Continuous inventory   ·   Partial fills grouped   ·   Fees and funding included', fontsize=10, color=MUTED)
    ax = fig.add_axes([.09, .16, .50, .44]); style_axis(ax)
    ax.set_title('Net PnL per completed basis trade', loc='left', fontsize=16, weight='bold', pad=24)
    ax.scatter(times, values, s=8, alpha=.6, linewidths=0, color=[GREEN if v>=0 else RED for v in values])
    money_axis(ax, values)
    ax.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=3, maxticks=4))
    ax.xaxis.set_major_formatter(mdates.DateFormatter('%b %d', tz=timezone.utc))
    ax.margins(x=.02, y=.15)
    ci = fig.add_axes([.77, .22, .19, .35]); ci.set_facecolor(SURFACE)
    ci.set_title('Mean trade PnL', loc='left', fontsize=16, weight='bold', pad=30)
    bounds = stats['t_test']['ci95_two_sided']
    ci.plot(bounds, [0, 0], lw=3, color=GREEN if bounds[0]>0 else RED if bounds[1]<0 else MUTED)
    ci.scatter([stats['mean']], [0], s=34, color=INK, zorder=3)
    ci.set_yticks([0], ['t-test'], fontsize=10); ci.set_ylim(.8, -.8)
    ci.axvline(0, color='#aab8c8', lw=.8); ci.tick_params(length=0, pad=9, labelsize=10)
    ci.text(0, 1.035, '95% confidence interval', transform=ci.transAxes, fontsize=10, color=MUTED)
    money_axis(ci, bounds, horizontal=True)
    fig.savefig(output, facecolor=SURFACE); plt.close(fig)


def finalize_event_trials(directory, report_dir):
    os.environ.setdefault('MPLCONFIGDIR', '/tmp/btc-event-matplotlib')
    directory, report_dir = directory.resolve(), report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    summary = json.loads((directory/'summary.json').read_text())
    provenance = json.loads((directory/'inputs.json').read_text())
    if not summary['single_basis_position'] or summary.get('daily_resets', False):
        raise ValueError('Event analysis requires the continuous one-position strategy')
    with (directory/'pairs.csv').open() as handle:
        events, incomplete = aggregate_events(list(csv.DictReader(handle)), summary)
    start, end = provenance['start_ms'], provenance['end_ms']
    report = dict(continuous_inventory=True, primary_inference='t_test', observation_unit='completed flat-to-flat basis trade',
        definition='Partial fills share an engine position ID; sum complete two-leg lifetime PnL, all executed entry/exit fees and realized funding. Unfilled signals are not trades.',
        hypothesis='H0: mean completed-trade net PnL <= 0; H1: mean > 0',
        start_date=datetime.fromtimestamp(start/1000, timezone.utc).strftime('%d %b %Y'),
        end_date=datetime.fromtimestamp((end-1)/1000, timezone.utc).strftime('%d %b %Y'),
        summary=summary, statistics=event_statistics(events, start, end), events=events, incomplete_positions=incomplete,
        incomplete_position_policy='Open positions at the archive end are excluded from the completed-trade test; their marked contribution remains in the portfolio result. No daily or terminal forced exits were added.',
        inference_note='Nonoverlapping trades are not necessarily independent. The requested ordinary t-test is an in-sample diagnostic; exit-day clustering/HAC only address some market dependence. Lifetime PnL per trade is not PnL per day or a fixed-capital return.',
        checks=dict(partial_fills_grouped=True, fees_reconciled=True, funding_reconciled=True,
                    completed_plus_open_pnl_matches_portfolio=True),
        provenance=dict(pairs_sha256=hashlib.sha256((directory/'pairs.csv').read_bytes()).hexdigest(),
            event_analysis_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()))
    with (report_dir/'events.csv').open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=list(events[0])); writer.writeheader(); writer.writerows(events)
    # Keep daily diagnostics, while the main trials export now tests trade events.
    for name in ('trials.json', 'trials.png'):
        target = report_dir/('daily-diagnostics'+Path(name).suffix)
        if (report_dir/name).exists() and not target.exists():
            shutil.copy2(report_dir/name, target)
    (report_dir/'trials.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    render_events(report, report_dir/'trials.png')
    for name in ('summary.json', 'validation.json', 'inputs.json', 'pairs.csv', 'equity.csv', 'coverage.csv'):
        shutil.copy2(directory/name, report_dir/name)
    print(f'Completed trades: {len(events):,}; net {report["statistics"]["total"]:+,.6f} USDT; one-sided t p={report["statistics"]["t_test"]["p_one_sided"]:.6g}; excluded open positions {len(incomplete)}', flush=True)
    return report
