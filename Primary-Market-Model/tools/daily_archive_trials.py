"""Validate and combine fresh daily trials with liquidated or marked endings."""
import csv
import copy
import hashlib
import json
import math
from datetime import datetime, timezone

from run_backtest import MODEL, render_png, render_report
from run_archive_backtest import verify_funding, verify_sides
from run_trials import analyze, render

DAY=86400000
SIDES=('long_perp_short_spot','short_perp_long_spot')
SUM_FIELDS=('entry_events','spot_proxy_events','spot_mid_events','paired_partial_fills','leg_fills',
            'price_pnl','fees','perp_maker_notional','perp_taker_notional','spot_taker_notional',
            'closed_pairs','winning_closed_pairs','losing_closed_pairs','realized_pnl',
            'unrealized_pnl','funding_pnl','net_pnl','gross_perp_btc',
            'funding_exit_pairs','convergence_exit_pairs','opposite_signal_exit_pairs','daily_trial_exit_pairs')
PNL_COLUMNS=('price_pnl','fees','funding_pnl','net_pnl','realized_pnl','unrealized_pnl','gross_perp_btc')+tuple(
    side+'_'+field for side in SIDES for field in
    ('unrealized_pnl','realized_price_pnl','funding_pnl','fees','net_pnl'))


def combine_trials(directory, trials, max_drawdown, close_at_day_end=True):
    """Add daily endpoints; quantities describe only the final trial's holdings."""
    summary=copy.deepcopy(trials[0]['summary'])
    for field in SUM_FIELDS:summary[field]=sum(t['summary'][field] for t in trials)
    for field in ('max_open_gross_perp_btc','max_entry_event_btc','max_concurrent_pairs'):
        summary[field]=max(t['summary'][field] for t in trials)
    for field in ('perp_btc','spot_btc','delta_btc','open_gross_perp_btc','open_pairs'):
        summary[field]=trials[-1]['summary'].get(field,0)
    for field in ('final_perp_mid','final_spot_mid'):summary[field]=trials[-1]['summary'][field]
    for side in SIDES:
        for field in summary['basis_sides'][side]:
            summary['basis_sides'][side][field]=(trials[-1]['summary']['basis_sides'][side][field]
                if field in ('open_pairs','open_btc') else sum(t['summary']['basis_sides'][side][field] for t in trials))
    ending=('both legs liquidated at day-end with taker fees' if close_at_day_end else
            'open positions marked at final midprices without exits or hypothetical closing fees')
    summary.update(daily_resets=True,close_at_day_end=close_at_day_end,trial_count=len(trials),max_drawdown=max_drawdown,
                   pnl_definition='Sum of separate flat-start UTC day trials; '+ending,
                   marked_trial_end_pairs=sum(t['summary'].get('open_pairs',0) for t in trials),
                   terminal_inventory_definition='Inventory fields describe the final trial only; financial PnL fields sum all daily endpoints.',
                   final_spot_notional=summary.get('spot_btc',0)*summary['final_spot_mid'])
    summary.pop('trial_date',None)
    summary.update(start_date=trials[0]['date'],end_date=trials[-1]['date'])
    summary['limitations']='Each daily trial starts flat; '+ending+'. Marked endings include signed unrealized gains AND losses; positions are not carried into the next trial. This sum of separate trial valuations is not one executable continuous portfolio. Spot uses a last-trade proxy with assumed spread and zero latency. Borrow availability, interest, impact, depth, margin and liquidation are not modeled. No queue reconstruction.'
    offsets={key:0.0 for key in PNL_COLUMNS}
    with (directory/'equity.csv').open('w',newline='') as output:
        writer=None
        for trial in trials:
            with (directory/'days'/trial['date']/'equity.csv').open() as handle:
                reader=csv.DictReader(handle)
                if writer is None:
                    writer=csv.DictWriter(output,fieldnames=reader.fieldnames);writer.writeheader()
                for row in reader:
                    for key in PNL_COLUMNS:row[key]=f'{float(row[key])+offsets[key]:.10f}'
                    writer.writerow(row)
                offsets={key:float(row[key]) for key in PNL_COLUMNS}
    if not math.isclose(offsets['net_pnl'],summary['net_pnl'],abs_tol=1e-6):
        raise ValueError('Cumulative daily curve does not match sum of trial outcomes')
    return summary


def finalize_daily_trials(args, manifests):
    directory=args.output;report_dir=args.report_dir
    report_dir.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((directory/'daily-trials.json').read_text())
    close_at_day_end=bool(args.close_at_day_end)
    if not manifest['daily_reset'] or manifest['close_at_day_end']!=close_at_day_end:
        raise ValueError('Daily engine reset or terminal policy differs from requested settings')
    expected_dates=[datetime.fromtimestamp(t/1000,timezone.utc).strftime('%Y-%m-%d')
                    for t in range(manifests[0]['start_ms'],manifests[-1]['end_ms'],DAY)]
    if manifest['dates']!=expected_dates:raise ValueError('Missing or extra daily trial')
    trials=[];validations=[]
    for day in manifest['dates']:
        folder=directory/'days'/day
        summary=json.loads((folder/'summary.json').read_text())
        start=int(datetime.strptime(day,'%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp()*1000)
        source=next(m for m in manifests if m['month']==day[:7])
        for field in ('equilibrium_bps','entry_offset_bps','perp_maker_bps','perp_taker_bps','spot_taker_bps'):
            if summary[field]!=getattr(args,field):raise ValueError(f'{day}: incorrect {field}')
        for field in ('one_order_per_entry_event','single_basis_position','exit_on_opposite_signal'):
            if summary[field]!=bool(getattr(args,field)):raise ValueError(f'{day}: incorrect {field}')
        if not summary['exit_before_funding'] or not summary['funding_exit_payers_only']:
            raise ValueError(f'{day}: funding-payer exits disabled')
        if summary['entry_band_violations'] or summary['convergence_exit_pairs'] or abs(summary['delta_btc'])>1e-9:
            raise ValueError(f'{day}: strategy or hedge violation')
        ends_flat=summary['open_gross_perp_btc']<1e-9
        if close_at_day_end and not ends_flat:
            raise ValueError(f'{day}: nonflat terminal inventory after requested liquidation')
        if not close_at_day_end and summary['daily_trial_exit_pairs']:
            raise ValueError(f'{day}: marked trial executed a day-end exit')
        if args.single_basis_position and summary['max_open_gross_perp_btc']>summary['order_btc']+1e-9:
            raise ValueError(f'{day}: one-trade position limit exceeded')
        if args.one_order_per_entry_event and summary['max_entry_event_btc']>summary['order_btc']+1e-9:
            raise ValueError(f'{day}: entry-event order budget exceeded')
        if summary['closed_pairs']!=summary['funding_exit_pairs']+summary['opposite_signal_exit_pairs']+summary['daily_trial_exit_pairs']:
            raise ValueError(f'{day}: exit reasons do not reconcile')
        fees=sum(summary[leg+'_notional']*summary[leg+'_bps']/10000 for leg in ('perp_maker','perp_taker','spot_taker'))
        if not math.isclose(fees,summary['fees'],abs_tol=1e-6):raise ValueError(f'{day}: fee reconciliation failed')
        if not math.isclose(summary['price_pnl']+summary['funding_pnl']-summary['fees'],summary['net_pnl'],abs_tol=1e-6):
            raise ValueError(f'{day}: PnL reconciliation failed')
        if not math.isclose(summary['realized_pnl']+summary['unrealized_pnl'],summary['net_pnl'],abs_tol=1e-6):
            raise ValueError(f'{day}: realized plus signed unrealized PnL fails reconciliation')
        if close_at_day_end and abs(summary['unrealized_pnl'])>1e-6:
            raise ValueError(f'{day}: liquidated trial retains unrealized PnL')
        with (folder/'equity.csv').open() as handle:
            for index,row in enumerate(csv.DictReader(handle)):
                if not start<=int(row['time_ms'])<start+DAY:raise ValueError(f'{day}: equity outside trial day')
                if index==0 and (abs(float(row['net_pnl']))>1e-9 or abs(float(row['open_gross_perp_btc']))>1e-9):
                    raise ValueError(f'{day}: trial did not start flat with zero PnL')
                if args.single_basis_position and min(float(row[side+'_open_btc']) for side in SIDES)>1e-9:
                    raise ValueError(f'{day}: simultaneous opposite positions')
        if int(row['time_ms'])!=start+DAY-1:raise ValueError(f'{day}: missing exact terminal observation')
        validation=verify_funding(folder,[source],start,start+DAY)
        validation.update(verify_sides(folder,summary))
        validation.update(starts_flat=True,ends_flat=ends_flat,day_end_fees_included=close_at_day_end,
                          marked_ending_without_exit=not close_at_day_end,
                          max_position_units=summary['max_open_gross_perp_btc']/summary['order_btc'])
        (folder/'validation.json').write_text(json.dumps(validation,indent=2)+'\n')
        summary.update(trial_date=day,daily_reset=True,close_at_day_end=close_at_day_end)
        (folder/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
        trials.append(dict(date=day,summary=summary));validations.append(validation)
    total=sum(t['summary']['net_pnl'] for t in trials)
    if not math.isclose(total,manifest['total_net_pnl'],abs_tol=1e-6):raise ValueError('Trial sum differs from engine manifest')
    summary=combine_trials(directory,trials,manifest['max_drawdown'],close_at_day_end)
    with (directory/'coverage.csv').open() as handle:coverage=list(csv.DictReader(handle))
    quote_rows=[row for row in coverage if row['kind']=='Q']
    summary['max_quote_gap_ms']=max(int(row['max_gap_ms']) for row in quote_rows)
    summary['quote_gap_days']=[dict(date=datetime.fromtimestamp(int(row['utc_day'])*86400,timezone.utc).strftime('%Y-%m-%d'),
                                    max_gap_ms=int(row['max_gap_ms'])) for row in quote_rows if int(row['max_gap_ms'])>10000]
    summary['funding_paid_usdt']=sum(v['gross_funding_paid_usdt'] for v in validations)
    summary['funding_received_usdt']=sum(v['gross_funding_received_usdt'] for v in validations)
    (directory/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    combined_validation=verify_sides(directory,summary)
    combined_validation.update(trial_count=len(trials),every_trial_starts_flat=True,
        every_trial_starts_and_ends_flat=all(v['ends_flat'] for v in validations),
        trials_ending_with_open_inventory=sum(not v['ends_flat'] for v in validations),
        terminal_exit_costs_included=close_at_day_end,marked_endings_without_exit=not close_at_day_end,
        funding_settlements=sum(v['funding_settlements'] for v in validations),
        gross_funding_paid_usdt=summary['funding_paid_usdt'],gross_funding_received_usdt=summary['funding_received_usdt'],
        max_position_units=max(v['max_position_units'] for v in validations),
        max_payment_error_usdt=max(v['max_payment_error_usdt'] for v in validations),
        sum_of_daily_pnl_verified=True)
    (directory/'validation.json').write_text(json.dumps(combined_validation,indent=2)+'\n')
    report=dict(continuous_inventory=False,independent_starting_inventory=True,primary_inference='t_test',
        trial_definition='Separate UTC days; fresh engine and zero inventory each midnight; '+summary['pnl_definition'].split('; ',1)[1],
        hypothesis='H0: mean daily net PnL <= 0; H1: mean > 0',
        interpretation='Threshold selected after inspecting historical sensitivity plots; ordinary t-test is an in-sample diagnostic, conditional on independence assumptions.',
        independence_note='Resetting positions removes inventory carryover but does not guarantee statistically independent or stationary market days.',
        methods=dict(primary='One-sided one-sample t-test across daily trial endpoints, df=n-1; 95% two-sided t interval',
                     robustness='Daily autocorrelation and Ljung-Box diagnostics; Newey-West HAC at 3/7 lags and centered circular block bootstrap at 3/7-day block lengths'),
        limitations=['Strategy and threshold selected using previously inspected history.',
                     'Daily resets do not remove common market regimes or serial dependence.',
                     'Spot last-trade proxy, assumed spread, zero latency and simplified maker fills; borrowing, impact, depth, margin and liquidation omitted.'],
        statistics=analyze([t['summary']['net_pnl'] for t in trials]),
        price_only_statistics=analyze([t['summary']['price_pnl']-t['summary']['fees'] for t in trials]),trials=trials,
        data_quality=dict(max_quote_gap_ms=summary['max_quote_gap_ms'],quote_gap_days=summary['quote_gap_days']))
    (report_dir/'trials.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    provenance=dict(start_ms=manifests[0]['start_ms'],end_ms=manifests[-1]['end_ms'],sources=manifests,
        daily_reset=True,close_at_day_end=close_at_day_end,days=manifest['dates'],
        settings={key:getattr(args,key) for key in ('equilibrium_bps','entry_offset_bps','perp_maker_bps',
                    'perp_taker_bps','spot_taker_bps','single_basis_position','one_order_per_entry_event','exit_on_opposite_signal')},
        engine_sha256=hashlib.sha256((MODEL/'build/archive-replay').read_bytes()).hexdigest(),
        archive_sort_sha256=hashlib.sha256((MODEL/'build/archive-sort').read_bytes()).hexdigest(),
        analysis_sha256={name:hashlib.sha256((MODEL/'tools'/name).read_bytes()).hexdigest()
                         for name in ('daily_archive_trials.py','run_trials.py','presentation.py','run_archive_backtest.py')})
    (directory/'inputs.json').write_text(json.dumps(provenance,indent=2)+'\n')
    with (report_dir/'daily-pnl.csv').open('w',newline='') as handle:
        writer=csv.DictWriter(handle,fieldnames=['date','net_pnl_usdt','price_pnl_usdt','funding_pnl_usdt','fees_usdt',
                                                'long_basis_net_pnl_usdt','short_basis_net_pnl_usdt','day_end_closed_partial_pairs',
                                                'realized_pnl_usdt','unrealized_pnl_usdt','terminal_open_perp_btc'])
        writer.writeheader()
        for trial in trials:
            s=trial['summary'];writer.writerow(dict(date=trial['date'],net_pnl_usdt=s['net_pnl'],price_pnl_usdt=s['price_pnl'],
                funding_pnl_usdt=s['funding_pnl'],fees_usdt=s['fees'],long_basis_net_pnl_usdt=s['basis_sides'][SIDES[0]]['net_pnl'],
                short_basis_net_pnl_usdt=s['basis_sides'][SIDES[1]]['net_pnl'],day_end_closed_partial_pairs=s['daily_trial_exit_pairs'],
                realized_pnl_usdt=s['realized_pnl'],unrealized_pnl_usdt=s['unrealized_pnl'],terminal_open_perp_btc=s['perp_btc']))
    render_report(directory,report_dir/'result.html');render_png(directory,report_dir/'result.png');render(report,report_dir/'trials.png')
    print(f'Flat-start daily trials: {len(trials)}; total net {total:+,.6f} USDT; one-sided t-test p={report["statistics"]["t_test"]["p_one_sided"]:.6g}',flush=True)
