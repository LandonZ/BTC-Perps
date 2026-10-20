"""Compact, 16:9 exports for the final presentation."""
import math
from datetime import datetime, timedelta, timezone

SURFACE = '#fbfcfe'
INK = '#172b42'
MUTED = '#64748b'
GREEN = '#087f6b'
RED = '#c44955'
GRID = '#e4eaf1'


def compact(value, signed=False):
    scale, suffix = (1e6, 'm') if abs(value) >= 1e6 else (1e3, 'k') if abs(value) >= 1e3 else (1, '')
    return (f'{value/scale:+,.2f}' if signed else f'{value/scale:,.2f}') + suffix


def setup():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.family':'DejaVu Sans', 'font.size':12,
                         'text.color':INK, 'axes.labelcolor':MUTED,
                         'xtick.color':MUTED, 'ytick.color':MUTED,
                         'axes.spines.top':False, 'axes.spines.right':False,
                         'axes.spines.left':False, 'axes.spines.bottom':False})
    return plt


def style_axis(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(axis='y', color=GRID, linewidth=.8)
    ax.axhline(0, color='#aab8c8', linewidth=.8)
    ax.tick_params(length=0, pad=9, labelsize=11)
    ax.set_axisbelow(True)


def money_axis(ax, values, horizontal=False):
    from matplotlib.ticker import FuncFormatter, MaxNLocator
    magnitude = max(abs(value) for value in values)
    scale, unit = (1e6, 'millions') if magnitude >= 1e6 else (1e3, 'thousands') if magnitude >= 1e3 else (1, '')
    axis = ax.xaxis if horizontal else ax.yaxis
    precision=2 if magnitude/scale<1 else 1
    axis.set_major_formatter(FuncFormatter(lambda v, pos: f'{v/scale:,.{precision}f}'))
    axis.set_major_locator(MaxNLocator(nbins=4))
    label = 'USDT' + (f' · {unit}' if unit else '')
    if horizontal: ax.set_xlabel(label, labelpad=11, fontsize=11)
    else: ax.set_ylabel(label, labelpad=10, fontsize=11)


def header(fig, title, period, settings, metrics):
    fig.text(.07, .935, title, fontsize=29, weight='bold')
    fig.text(.07, .891, period, fontsize=12, color=MUTED)
    fig.text(.07, .852, settings, fontsize=11, color=MUTED)
    for x, label, value, color in metrics:
        fig.text(x, .782, label, fontsize=10, color=MUTED)
        fig.text(x, .723, value, fontsize=31, weight='bold', color=color)


def settings_line(s):
    text = (f'Long ≤ {s["entry_bid_basis_bps"]:g} bp   ·   Short ≥ {s["entry_ask_basis_bps"]:g} bp'
            f'   |   Maker {s["perp_maker_bps"]:g} bp   ·   Taker {s["spot_taker_bps"]:g} bp')
    if s.get('single_basis_position'):
        text += f'   |   Position −1 / +1   ·   1 trade = {s.get("order_btc",.01):g} BTC'
    return text


def render_directions(summary, rows, output):
    plt = setup()
    import matplotlib.dates as dates
    times = [datetime.fromtimestamp(int(row['time_ms'])/1000, timezone.utc) for row in rows]
    fig = plt.figure(figsize=(16, 9), dpi=240, facecolor=SURFACE)
    header(fig, 'BTC basis strategy',
           f'Binance BTCUSDT   ·   {times[0]:%d %b %Y} – {times[-1]:%d %b %Y} UTC',
           settings_line(summary),
           [(.07, 'NET PNL · USDT', compact(summary['net_pnl'], True), INK),
            (.39, 'FUNDING · USDT', compact(summary['funding_pnl'], True), GREEN),
            (.71, 'PRICE PNL AFTER FEES · USDT', compact(summary['price_pnl']-summary['fees'], True),
             GREEN if summary['price_pnl']-summary['fees'] >= 0 else RED)])
    for left, name, title, color in [(.09, 'long_perp_short_spot', 'Long perp / short spot', GREEN),
                                      (.58, 'short_perp_long_spot', 'Short perp / long spot', '#3568a8')]:
        ax = fig.add_axes([left, .14, .37, .465])
        side = summary['basis_sides'][name]
        total = [float(row[name+'_net_pnl']) for row in rows]
        if not math.isclose(total[-1], side['net_pnl'], abs_tol=1e-5):
            raise ValueError('Presentation curve does not reconcile to direction PnL')
        style_axis(ax)
        ax.plot(times, total, color=color, lw=1.65)
        if not side['entries']:
            ax.text(.5,.62,'No entries',transform=ax.transAxes,ha='center',fontsize=13,color=MUTED)
        money_axis(ax, total)
        ax.set_title(title, loc='left', fontsize=16, weight='bold', color=color, pad=34)
        share=f'   ·   {100*side["net_pnl"]/summary["net_pnl"]:.2f}% of total' if abs(summary['net_pnl'])>1e-8 else ''
        ax.text(0, 1.035, f'Net PnL  {side["net_pnl"]:+,.2f} USDT'+share,
                transform=ax.transAxes, fontsize=11, color=MUTED)
        ax.xaxis.set_major_locator(dates.AutoDateLocator(minticks=3, maxticks=4))
        ax.xaxis.set_major_formatter(dates.DateFormatter('%b %d', tz=timezone.utc))
        ax.set_xlim(times[0], times[-1]); ax.margins(y=.15)
    output.parent.mkdir(parents=True,exist_ok=True)
    fig.savefig(output, facecolor=SURFACE); plt.close(fig)


def render_trials(report, output):
    plt = setup()
    import matplotlib.dates as dates
    trials, stats = report['trials'], report['statistics']
    s = trials[0]['summary']
    pnl = [r['summary']['net_pnl'] for r in trials]
    days = [datetime.strptime(r['date'], '%Y-%m-%d').replace(tzinfo=timezone.utc) for r in trials]
    fig = plt.figure(figsize=(16,9), dpi=240, facecolor=SURFACE)
    header(fig, 'Daily performance',
           f'Binance BTCUSDT   ·   {days[0]:%d %b %Y} – {days[-1]:%d %b %Y} UTC   ·   {len(days)} days',
           settings_line(s),
           [(.07, 'MEAN DAILY PNL · USDT', compact(stats['mean'], True), INK),
            (.39, 'POSITIVE DAYS', f'{stats["positive"]} / {stats["n"]}', GREEN),
            (.71, 'ONE-SIDED t-TEST p-VALUE', f'{stats["t_test"]["p_one_sided"]:.2g}', INK)])
    ax=fig.add_axes([.09,.16,.50,.44]); style_axis(ax)
    ax.set_title('Daily net PnL, including funding',loc='left',fontsize=16,weight='bold',pad=24)
    ax.bar(days,pnl,width=.77,color=[GREEN if p>=0 else RED for p in pnl])
    money_axis(ax,pnl)
    ax.xaxis.set_major_locator(dates.AutoDateLocator(minticks=3,maxticks=4))
    ax.xaxis.set_major_formatter(dates.DateFormatter('%b %d',tz=timezone.utc))
    ax.margins(x=.015,y=.16)
    ax.set_xlim(days[0]-timedelta(days=.6),days[-1]+timedelta(days=.6))
    ci_ax=fig.add_axes([.77,.22,.19,.35]); ci_ax.set_facecolor(SURFACE)
    ci_ax.set_title('Mean daily PnL',loc='left',fontsize=16,weight='bold',pad=30)
    intervals=[stats['t_test']['ci95_two_sided']]
    for i,bounds in enumerate(intervals):
        color=GREEN if bounds[0]>0 else RED if bounds[1]<0 else MUTED
        ci_ax.plot(bounds,[i,i],color=color,lw=3,solid_capstyle='round')
        ci_ax.scatter([stats['mean']],[i],s=34,color=INK,zorder=3)
    ci_ax.set_yticks([0],['t-test'],fontsize=10)
    ci_ax.set_ylim(.8,-.8); ci_ax.axvline(0,color='#aab8c8',lw=.8)
    ci_ax.tick_params(length=0,pad=9,labelsize=10)
    money_axis(ci_ax,[v for bounds in intervals for v in bounds],horizontal=True)
    ci_ax.text(0,1.035,'95% confidence interval',transform=ci_ax.transAxes,fontsize=10,color=MUTED)
    output.parent.mkdir(parents=True,exist_ok=True)
    fig.savefig(output,facecolor=SURFACE);plt.close(fig)
