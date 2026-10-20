#!/usr/bin/env python3
"""Compare saved basis targets and entry offsets on identical data and execution rules."""
import argparse
import csv
import json
import math
import os
from datetime import datetime, timezone
from pathlib import Path

from run_backtest import MODEL


def compare(baseline, experiment, output):
    directories=(baseline,experiment)
    summaries=[json.loads((directory/'summary.json').read_text()) for directory in directories]
    inputs=[json.loads((directory/'inputs.json').read_text()) for directory in directories]
    for key in ('start_ms','end_ms'):
        if inputs[0][key]!=inputs[1][key]:raise ValueError('Hypotheses use different date windows')
    def source_hashes(provenance):
        return sorted((month['month'],name,source['sha256']) for month in provenance['sources']
                      for name,source in month['sources'].items())
    if source_hashes(inputs[0])!=source_hashes(inputs[1]):raise ValueError('Hypotheses use different source data')
    if summaries[0].get('one_order_per_entry_event',False)!=summaries[1].get('one_order_per_entry_event',False):
        raise ValueError('Additional strategy change: one_order_per_entry_event')
    if summaries[0].get('single_basis_position',False)!=summaries[1].get('single_basis_position',False):
        raise ValueError('Additional strategy change: single_basis_position')
    for key in ('latency_ms','perp_maker_bps','perp_taker_bps','spot_taker_bps',
                'funding_cancel_seconds','exit_on_equilibrium','exit_before_funding','funding_exit_payers_only',
                'funding_exit_flat_region_exempt','funding_cashflows','funding_interest_bps','funding_clamp_bps',
                'order_btc','max_gross_perp_btc','participation','fill_at_touch','proxy_spot_half_spread_bps'):
        if summaries[0][key]!=summaries[1][key]:raise ValueError(f'Additional strategy change: {key}')
    rows=[]
    for directory,summary in zip(directories,summaries):
        with (directory/'equity.csv').open() as handle:
            points=[(int(row['time_ms']),float(row['net_pnl']),float(row['price_pnl'])-float(row['fees']))
                    for row in csv.DictReader(handle)]
        if not points or not math.isclose(points[-1][1],summary['net_pnl'],abs_tol=1e-6):
            raise ValueError('Saved equity does not reconcile')
        if not math.isclose(points[-1][2],summary['price_pnl']-summary['fees'],abs_tol=1e-6):
            raise ValueError('Saved price PnL does not reconcile')
        if not math.isclose(summary['net_pnl'],summary['price_pnl']-summary['fees']+summary['funding_pnl'],abs_tol=1e-6):
            raise ValueError('Price PnL and funding do not sum to net PnL')
        if not all(math.isfinite(value) for point in points for value in point):raise ValueError('Nonfinite equity')
        rows.append(points)
    if [point[0] for point in rows[0]]!=[point[0] for point in rows[1]]:
        raise ValueError('Hypotheses do not use the same observation clock')
    if (baseline/'coverage.csv').read_bytes()!=(experiment/'coverage.csv').read_bytes():
        raise ValueError('Hypotheses do not cover the same market events')
    comparison=dict(baseline=str(baseline.resolve()),experiment=str(experiment.resolve()),
        start_ms=inputs[0]['start_ms'],end_ms=inputs[0]['end_ms'],same_data_and_execution_rules=True,
        interpretation='Exploratory comparison on previously inspected history; changing inventory sizes mean dollar PnLs are not equal-capital returns',
        hypotheses=[dict(target_bps=s['equilibrium_bps'],entry_offset_bps=s['entry_offset_bps'],entry_bid_basis_bps=s['entry_bid_basis_bps'],
            entry_ask_basis_bps=s['entry_ask_basis_bps'],price_pnl_after_fees=s['price_pnl']-s['fees'],
            funding_pnl=s['funding_pnl'],net_pnl=s['net_pnl'],fees=s['fees'],
            funding_paid_usdt=s['funding_paid_usdt'],entries=s['paired_partial_fills'],
            peak_open_btc_per_leg=s['max_open_gross_perp_btc'],final_open_btc_per_leg=s['open_gross_perp_btc'],
            long_price_pnl=sum(s['basis_sides']['long_perp_short_spot'][key] for key in ('unrealized_pnl','realized_price_pnl')),
            short_price_pnl=sum(s['basis_sides']['short_perp_long_spot'][key] for key in ('unrealized_pnl','realized_price_pnl')))
            for s in summaries])
    comparison['experiment_minus_baseline']={key:comparison['hypotheses'][1][key]-comparison['hypotheses'][0][key]
        for key in ('price_pnl_after_fees','funding_pnl','net_pnl','fees','entries','peak_open_btc_per_leg')}
    os.environ.setdefault('MPLCONFIGDIR',str(MODEL/'build/matplotlib-cache'))
    os.environ.setdefault('XDG_CACHE_HOME',str(MODEL/'build/cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.dates as dates
    from matplotlib.ticker import FuncFormatter
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,
        'axes.spines.top':False,'axes.spines.right':False,'axes.spines.left':False,'axes.spines.bottom':False})
    fig,axes=plt.subplots(2,1,figsize=(12,11.6),dpi=170,facecolor='#fafbfc')
    fig.subplots_adjust(left=.115,right=.96,top=.78,bottom=.365,hspace=.5)
    targets=[f'{summary["equilibrium_bps"]:g} bp target' for summary in summaries]
    if summaries[0]['entry_offset_bps'] != summaries[1]['entry_offset_bps']:
        targets=[f'{summary["equilibrium_bps"]:g} bp target / {summary["entry_offset_bps"]:g} bp offset' for summary in summaries]
    colors=('#77869c','#08765f')
    first,last=[datetime.fromtimestamp(rows[0][index][0]/1000,timezone.utc) for index in (0,-1)]
    fig.text(.075,.951,'BTC basis entries: controlled comparison',fontsize=24,fontweight='bold')
    fig.text(.075,.918,f'Binance BTCUSDT | {first:%Y-%m-%d} to {last:%Y-%m-%d} UTC | Zero latency',color='#52656e')
    rules=summaries[0]
    fig.text(.075,.891,f'Maker {rules["perp_maker_bps"]:g} / perp taker {rules["perp_taker_bps"]:g} / spot taker {rules["spot_taker_bps"]:g} bps. Both legs exit before any expected funding payment.',fontsize=9,color='#52656e')
    for y,summary,color in zip((.865,.843),summaries,colors):
        fig.text(.075,y,f'Target {summary["equilibrium_bps"]:g} bps: long perp at/below {summary["entry_bid_basis_bps"]:g}; short perp at/above {summary["entry_ask_basis_bps"]:g} bps.',fontsize=9,color=color)
    for ax,column,title in zip(axes,(2,1),('Price PnL after execution fees · funding excluded','Total net PnL · price PnL plus funding')):
        magnitude=max(abs(point[column]) for points in rows for point in points)
        divisor=1e6 if magnitude>=1e6 else 1
        ax.set_facecolor('#fafbfc')
        for points,label,color in zip(rows,targets,colors):
            times=[datetime.fromtimestamp(point[0]/1000,timezone.utc) for point in points]
            ax.plot(times,[point[column] for point in points],label=label,color=color,lw=1.3)
        ax.set_title(title,loc='left',fontsize=12,fontweight='bold',pad=15)
        ax.axhline(0,color='#aab8bf',lw=.7);ax.grid(axis='y',color='#dce4e8',lw=.7)
        ax.set_ylabel('USDT · millions' if divisor>1 else 'USDT',labelpad=12)
        ax.yaxis.set_major_formatter(FuncFormatter(lambda value,pos,d=divisor:f'{value/d:,.1f}'))
        ax.xaxis.set_major_locator(dates.AutoDateLocator(minticks=4,maxticks=6))
        ax.xaxis.set_major_formatter(dates.DateFormatter('%b %d',tz=timezone.utc))
        ax.set_xlim(first,last);ax.tick_params(length=0,pad=8);ax.margins(y=.12)
        ax.legend(loc='best',frameon=False,ncol=2)
    metrics=[('Price PnL after fees · USDT','price_pnl_after_fees'),('Funding cash · USDT','funding_pnl'),
             ('Total net PnL · USDT','net_pnl'),('Funding paid · USDT','funding_paid_usdt'),
             ('Long-perp paired price PnL · USDT','long_price_pnl'),('Short-perp paired price PnL · USDT','short_price_pnl'),
             ('Peak gross BTC per leg','peak_open_btc_per_leg'),('Entry partial-fill pairs','entries')]
    cells=[]
    for label,key in metrics:
        values=[hypothesis[key] for hypothesis in comparison['hypotheses']]
        cells.append([label]+[f'{value:,.0f}' if key=='entries' else f'{value:,.3f}' if key=='peak_open_btc_per_leg' else f'{value:+,.2f}' for value in values])
    table_ax=fig.add_axes([.075,.115,.885,.19]);table_ax.axis('off')
    table=table_ax.table(cellText=cells,colLabels=['Final result']+targets,colWidths=[.46,.27,.27],cellLoc='right',loc='center')
    table.auto_set_font_size(False);table.set_fontsize(9);table.scale(1,1.45)
    for (row,column),cell in table.get_celld().items():
        cell.set_edgecolor('#dce4e8');cell.set_linewidth(.5);cell.set_facecolor('#edf2f5' if row==0 else '#fafbfc')
        if row==0:cell.set_text_props(weight='bold')
        if column==0:cell.set_text_props(ha='left')
    fig.text(.075,.32,'Same source events, fee rates, fill model and funding-exit policy. Entry target and/or offset changes.',fontsize=9,color='#52656e')
    fig.text(.075,.078,'Unlimited inventory: different entry settings grow different exposures. Dollar PnLs are not returns on equal capital.',fontsize=9,color='#52656e')
    fig.text(.075,.054,'Previously inspected history. Open positions marked at mids; no terminal liquidation. Spot trade-price proxy; 1 bp full spread.',fontsize=9,color='#52656e')
    gap=max(summary.get('max_quote_gap_ms',0) for summary in summaries)/60000
    fig.text(.075,.03,f'Quote gaps up to {gap:.1f} min can distort interim marks. Queue, impact, depth, borrow, margin and liquidation omitted.',fontsize=9,color='#52656e')
    output.parent.mkdir(parents=True,exist_ok=True);fig.savefig(output,facecolor=fig.get_facecolor());plt.close(fig)
    output.with_suffix('.json').write_text(json.dumps(comparison,indent=2)+'\n')
    return comparison


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline',type=Path);parser.add_argument('experiment',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();result=compare(args.baseline,args.experiment,args.output)
    print(json.dumps(result['experiment_minus_baseline'],indent=2))


if __name__=='__main__':main()
