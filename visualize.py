import argparse
import json
import os
import sys
from datetime import datetime

import matplotlib
matplotlib.use('Agg')  # headless
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.gridspec import GridSpec
import numpy as np


# ─── Color palette: dark "Bloomberg terminal" aesthetic ─────────────
PALETTE = {
    'bg':        '#0a0e17',
    'panel':     '#0f1520',
    'grid':      '#1a2332',
    'text':      '#c8d6e5',
    'text_dim':  '#546e7a',
    'accent':    '#e8eaf6',
    'green':     '#00e676',
    'red':       '#ef5350',
    'yellow':    '#ffa726',
    'blue':      '#42a5f5',
    'cyan':      '#26c6da',
    'purple':    '#ab47bc',
    'orange':    '#ff7043',
}

EVENT_COLORS = {
    'Fill':        PALETTE['green'],
    'PartialFill': '#69f0ae',
    'Accepted':    PALETTE['blue'],
    'Rejected':    PALETTE['red'],
    'Cancelled':   PALETTE['yellow'],
}

CMD_COLORS = {
    'Add':    PALETTE['blue'],
    'Cancel': PALETTE['yellow'],
    'Modify': PALETTE['purple'],
}


def setup_style():
    """Apply dark theme to matplotlib globally."""
    plt.rcParams.update({
        'figure.facecolor':  PALETTE['bg'],
        'axes.facecolor':    PALETTE['panel'],
        'axes.edgecolor':    PALETTE['grid'],
        'axes.labelcolor':   PALETTE['text_dim'],
        'axes.titlecolor':   PALETTE['accent'],
        'axes.titlesize':    10,
        'axes.titleweight':  'bold',
        'axes.labelsize':    8,
        'axes.grid':         True,
        'grid.color':        PALETTE['grid'],
        'grid.linewidth':    0.5,
        'grid.alpha':        0.5,
        'xtick.color':       PALETTE['text_dim'],
        'ytick.color':       PALETTE['text_dim'],
        'xtick.labelsize':   7,
        'ytick.labelsize':   7,
        'text.color':        PALETTE['text'],
        'font.family':       'monospace',
        'font.size':         8,
        'legend.facecolor':  PALETTE['panel'],
        'legend.edgecolor':  PALETTE['grid'],
        'legend.fontsize':   7,
        'savefig.facecolor': PALETTE['bg'],
        'savefig.edgecolor': PALETTE['bg'],
    })


def fmt_ns(ns):
    """Format a nanosecond value for display."""
    if ns is None:
        return '—'
    if ns >= 1e9:
        return f'{ns/1e9:.2f}s'
    if ns >= 1e6:
        return f'{ns/1e6:.2f}ms'
    if ns >= 1e3:
        return f'{ns/1e3:.1f}μs'
    return f'{ns:.0f}ns'


def fmt_num(n):
    """Format a large number with thousand separators."""
    if n is None:
        return '—'
    if abs(n) >= 1e9:
        return f'{n/1e9:.2f}B'
    if abs(n) >= 1e6:
        return f'{n/1e6:.2f}M'
    if abs(n) >= 1e3:
        return f'{n/1e3:.1f}K'
    return f'{n:.0f}'


def fmt_inr(paise):
    """Format a paise value as INR."""
    if paise is None:
        return '—'
    rupees = paise / 100
    if rupees >= 1e7:
        return f'₹{rupees/1e7:.2f} Cr'
    if rupees >= 1e5:
        return f'₹{rupees/1e5:.2f} L'
    if rupees >= 1e3:
        return f'₹{rupees/1e3:.2f} K'
    return f'₹{rupees:.2f}'


# ─── Individual chart panels ────────────────────────────────────────

def panel_latency_percentiles(ax, summary):
    """Bar chart of submit vs E2E latency percentiles."""
    sub = summary.get('submit_latency', {})
    e2e = summary.get('e2e_latency', {})
    percentiles = ['p50_ns', 'p90_ns', 'p95_ns', 'p99_ns', 'p999_ns']
    labels = ['P50', 'P90', 'P95', 'P99', 'P99.9']

    sub_vals = [sub.get(p, 0) for p in percentiles]
    e2e_vals = [e2e.get(p, 0) for p in percentiles]

    x = np.arange(len(labels))
    width = 0.38

    b1 = ax.bar(x - width/2, sub_vals, width, label='submit',
                color=PALETTE['green'], edgecolor=PALETTE['green'], alpha=0.85)
    b2 = ax.bar(x + width/2, e2e_vals, width, label='end-to-end',
                color=PALETTE['blue'], edgecolor=PALETTE['blue'], alpha=0.85)

    ax.set_yscale('log')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel('nanoseconds (log)')
    ax.set_title('LATENCY PERCENTILES', loc='left', pad=12)
    ax.legend(loc='upper left', frameon=True)

    # Annotate bars with formatted values
    for bars, vals in [(b1, sub_vals), (b2, e2e_vals)]:
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width()/2, v * 1.1,
                        fmt_ns(v), ha='center', va='bottom',
                        fontsize=6, color=PALETTE['text'])


def panel_throughput_timeline(ax, timeline):
    """Line + area chart of commands/sec over time (smoothed)."""
    if not timeline:
        ax.text(0.5, 0.5, 'no throughput data', ha='center', va='center',
                transform=ax.transAxes, color=PALETTE['text_dim'])
        ax.set_title('THROUGHPUT OVER TIME (20ms rolling window)', loc='left', pad=12)
        return

    # Build per-ms command counts
    max_t = max(e['t_ms'] for e in timeline)
    cmds_per_ms = np.zeros(max_t + 1, dtype=np.int64)
    for e in timeline:
        cmds_per_ms[e['t_ms']] = e['cmds']

    # Rolling window for smoothed rate (commands/sec in a 20ms window)
    window_ms = 20
    if len(cmds_per_ms) >= window_ms:
        kernel = np.ones(window_ms) / (window_ms / 1000.0)  # gives cmds/sec
        rolling_rate = np.convolve(cmds_per_ms, kernel / window_ms, mode='same')
    else:
        rolling_rate = cmds_per_ms * 1000.0

    times = np.arange(len(rolling_rate))

    ax.fill_between(times, 0, rolling_rate,
                     color=PALETTE['green'], alpha=0.25)
    ax.plot(times, rolling_rate,
            color=PALETTE['green'], linewidth=1.3)

    # Mean line (across non-zero buckets)
    active_mask = cmds_per_ms > 0
    if active_mask.any():
        total_active_ms = active_mask.sum()
        total_cmds = cmds_per_ms.sum()
        overall_rate = total_cmds / (total_active_ms / 1000.0) if total_active_ms > 0 else 0
        ax.axhline(overall_rate, color=PALETTE['yellow'],
                   linestyle='--', linewidth=0.9, alpha=0.75,
                   label=f'overall {fmt_num(overall_rate)}/s')
        ax.legend(loc='upper right', frameon=True)

    ax.set_xlabel('time (ms)')
    ax.set_ylabel('commands/sec')
    ax.set_title(f'THROUGHPUT OVER TIME ({window_ms}ms rolling window)',
                 loc='left', pad=12)
    ax.set_xlim(0, max_t)
    # Human-readable y-axis
    ax.yaxis.set_major_formatter(
        matplotlib.ticker.FuncFormatter(lambda x, _: fmt_num(x)))


def panel_latency_cdf(ax, commands):
    """Cumulative distribution function of submit latency."""
    if not commands:
        ax.text(0.5, 0.5, 'no latency data', ha='center', va='center',
                transform=ax.transAxes, color=PALETTE['text_dim'])
        ax.set_title('LATENCY CDF', loc='left', pad=12)
        return

    lat = np.array([c.get('submit_latency_ns', 0) for c in commands
                    if c.get('submit_latency_ns', 0) > 0])
    if len(lat) == 0:
        return

    sorted_lat = np.sort(lat)
    # Use quantile probabilities (0, 1/n, 2/n, ..., 1)
    probs = np.arange(1, len(sorted_lat) + 1) / len(sorted_lat)

    ax.plot(sorted_lat, probs * 100,
            color=PALETTE['cyan'], linewidth=1.5)
    ax.fill_between(sorted_lat, 0, probs * 100,
                     color=PALETTE['cyan'], alpha=0.15)

    # Mark key percentiles
    for pct, color in [(50, PALETTE['green']),
                        (95, PALETTE['yellow']),
                        (99, PALETTE['orange']),
                        (99.9, PALETTE['red'])]:
        val = np.percentile(lat, pct)
        ax.axvline(val, color=color, linestyle='--', linewidth=0.8, alpha=0.7)
        ax.text(val, pct, f' p{pct}\n {fmt_ns(val)}',
                color=color, fontsize=6,
                verticalalignment='center')

    ax.set_xscale('log')
    ax.set_xlabel('latency (ns, log)')
    ax.set_ylabel('percentile (%)')
    ax.set_title('SUBMIT LATENCY CDF', loc='left', pad=12)
    ax.set_ylim(0, 100)
    ax.set_yticks([0, 25, 50, 75, 90, 95, 99, 100])


def panel_latency_histogram(ax, commands):
    """Histogram of submit latency distribution."""
    if not commands:
        ax.text(0.5, 0.5, 'no latency data', ha='center', va='center',
                transform=ax.transAxes, color=PALETTE['text_dim'])
        ax.set_title('SUBMIT LATENCY DISTRIBUTION', loc='left', pad=12)
        return

    lat = [c.get('submit_latency_ns', 0) for c in commands if c.get('submit_latency_ns', 0) > 0]
    if not lat:
        return

    # Clip upper tail at p99 for readable histogram
    lat_arr = np.array(lat)
    p99 = np.percentile(lat_arr, 99)
    clipped = lat_arr[lat_arr <= p99]

    n, bins, patches = ax.hist(clipped, bins=60,
                                color=PALETTE['blue'],
                                edgecolor=PALETTE['blue'],
                                alpha=0.75)

    # Annotate with percentile lines
    p50 = np.percentile(lat_arr, 50)
    p95 = np.percentile(lat_arr, 95)
    ax.axvline(p50, color=PALETTE['green'], linestyle='--',
               linewidth=1, label=f'p50 {fmt_ns(p50)}')
    ax.axvline(p95, color=PALETTE['yellow'], linestyle='--',
               linewidth=1, label=f'p95 {fmt_ns(p95)}')

    ax.set_xlabel('latency (ns)')
    ax.set_ylabel('count')
    ax.set_title('LATENCY HISTOGRAM (clipped @ p99)',
                 loc='left', pad=12)
    ax.legend(loc='upper right', frameon=True)


def panel_latency_scatter(ax, commands, max_points=5000):
    """Scatter of latency over time, colored by command type."""
    if not commands:
        return

    # Uniform downsample across entire time range
    if len(commands) > max_points:
        indices = np.linspace(0, len(commands) - 1, max_points, dtype=int)
        sample = [commands[i] for i in indices]
    else:
        sample = commands

    for cmd_type, color in CMD_COLORS.items():
        xs = [c['submit_wall_ns'] / 1e6 for c in sample if c['type'] == cmd_type]
        ys = [c['submit_latency_ns'] for c in sample if c['type'] == cmd_type and c['submit_latency_ns'] > 0]
        xs = xs[:len(ys)]
        if xs:
            ax.scatter(xs, ys, s=3, c=color, alpha=0.5, label=cmd_type,
                       edgecolors='none')

    ax.set_yscale('log')
    ax.set_xlabel('time (ms)')
    ax.set_ylabel('submit latency (ns, log)')
    ax.set_title('LATENCY JITTER OVER TIME (by command type)',
                 loc='left', pad=12)
    ax.legend(loc='upper right', frameon=True, markerscale=3)
    # Ensure x-axis spans full run
    if commands:
        max_t = max(c['submit_wall_ns'] for c in commands) / 1e6
        ax.set_xlim(0, max_t * 1.02)


def panel_event_breakdown(ax, summary):
    """Horizontal stacked bar of event type proportions."""
    events = [
        ('Accepted',    summary.get('accepted', 0),    EVENT_COLORS['Accepted']),
        ('Fill',        summary.get('fills', 0),       EVENT_COLORS['Fill']),
        ('PartialFill', summary.get('partial_fills', 0), EVENT_COLORS['PartialFill']),
        ('Cancelled',   summary.get('cancelled', 0),   EVENT_COLORS['Cancelled']),
        ('Rejected',    summary.get('rejected', 0),    EVENT_COLORS['Rejected']),
    ]
    total = sum(e[1] for e in events)
    if total == 0:
        ax.text(0.5, 0.5, 'no events', ha='center', va='center',
                transform=ax.transAxes, color=PALETTE['text_dim'])
        ax.set_title('EVENT BREAKDOWN', loc='left', pad=12)
        return

    labels = [e[0] for e in events]
    values = [e[1] for e in events]
    colors = [e[2] for e in events]

    y_pos = np.arange(len(labels))
    bars = ax.barh(y_pos, values, color=colors, edgecolor=colors, alpha=0.85)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlabel('count')
    ax.set_title('EVENT BREAKDOWN', loc='left', pad=12)

    for bar, v in zip(bars, values):
        pct = 100 * v / total
        ax.text(bar.get_width() + max(values) * 0.01,
                bar.get_y() + bar.get_height()/2,
                f'{fmt_num(v)}  ({pct:.1f}%)',
                va='center', fontsize=7, color=PALETTE['text'])
    ax.set_xlim(0, max(values) * 1.25)


def panel_command_type_latency(ax, commands):
    """Box plot of latency grouped by command type."""
    if not commands:
        return

    data = {'Add': [], 'Cancel': [], 'Modify': []}
    for c in commands:
        t = c.get('type')
        lat = c.get('submit_latency_ns', 0)
        if t in data and lat > 0:
            data[t].append(lat)

    labels = [k for k in data if data[k]]
    arrays = [data[k] for k in labels]
    if not arrays:
        return

    bp = ax.boxplot(arrays, tick_labels=labels, patch_artist=True,
                     showfliers=False, widths=0.55,
                     medianprops=dict(color=PALETTE['accent'], linewidth=1.5),
                     whiskerprops=dict(color=PALETTE['text_dim']),
                     capprops=dict(color=PALETTE['text_dim']))
    for patch, label in zip(bp['boxes'], labels):
        patch.set_facecolor(CMD_COLORS.get(label, PALETTE['blue']))
        patch.set_alpha(0.7)
        patch.set_edgecolor(CMD_COLORS.get(label, PALETTE['blue']))

    ax.set_yscale('log')
    ax.set_ylabel('latency (ns, log)')
    ax.set_title('LATENCY BY COMMAND TYPE', loc='left', pad=12)


def panel_book_spread(ax, snapshots, watch_instrument=None):
    """Line chart of bid/ask/spread for watched instrument(s)."""
    if not snapshots:
        ax.text(0.5, 0.5, 'no book snapshots', ha='center', va='center',
                transform=ax.transAxes, color=PALETTE['text_dim'])
        ax.set_title('TOP OF BOOK', loc='left', pad=12)
        return

    # Group by instrument
    by_inst = {}
    for s in snapshots:
        iid = s['instrument_id']
        by_inst.setdefault(iid, []).append(s)

    # If watch set and exists, use it; else pick instrument with most snapshots
    if watch_instrument and watch_instrument in by_inst:
        target = watch_instrument
    else:
        target = max(by_inst, key=lambda k: len(by_inst[k]))

    snaps = by_inst[target]
    times = [s['wall_ns'] / 1e6 for s in snaps]
    bids = [s['best_bid'] / 100 if s['best_bid'] >= 0 else None for s in snaps]
    asks = [s['best_ask'] / 100 if s['best_ask'] >= 0 else None for s in snaps]

    # Filter out None
    bt = [(t, b) for t, b in zip(times, bids) if b is not None]
    at = [(t, a) for t, a in zip(times, asks) if a is not None]

    if bt:
        tb, bb = zip(*bt)
        ax.plot(tb, bb, color=PALETTE['green'], linewidth=1.3,
                label='bid', marker='o', markersize=2)
    if at:
        ta, aa = zip(*at)
        ax.plot(ta, aa, color=PALETTE['red'], linewidth=1.3,
                label='ask', marker='o', markersize=2)

    ax.set_xlabel('time (ms)')
    ax.set_ylabel('price (₹)')
    ax.set_title(f'TOP OF BOOK — INSTRUMENT {target}', loc='left', pad=12)
    ax.legend(loc='best', frameon=True)


def panel_instrument_activity(ax, commands):
    """Vertical bar chart of commands per instrument."""
    if not commands:
        return

    counts = {}
    for c in commands:
        iid = c.get('instrument_id')
        counts[iid] = counts.get(iid, 0) + 1

    if not counts:
        return

    sorted_items = sorted(counts.items(), key=lambda x: x[0])  # sort by id
    ids = [str(i[0]) for i in sorted_items]
    vals = [i[1] for i in sorted_items]

    colors_cycle = [PALETTE['blue'], PALETTE['cyan'], PALETTE['purple'],
                    PALETTE['orange'], PALETTE['green']]
    bar_colors = [colors_cycle[i % len(colors_cycle)] for i in range(len(ids))]

    x = np.arange(len(ids))
    bars = ax.bar(x, vals, color=bar_colors, alpha=0.85,
                   edgecolor=bar_colors)
    ax.set_xticks(x)
    ax.set_xticklabels([f'#{i}' for i in ids], rotation=0)
    ax.set_ylabel('commands')
    ax.set_xlabel('instrument id')
    ax.set_title('ACTIVITY BY INSTRUMENT', loc='left', pad=12)

    for bar, v in zip(bars, vals):
        ax.text(bar.get_x() + bar.get_width()/2,
                bar.get_height() + max(vals)*0.01,
                fmt_num(v),
                ha='center', va='bottom', fontsize=6, color=PALETTE['text'])


def panel_summary_text(ax, summary, config, instruments):
    """Summary statistics as text."""
    ax.axis('off')
    sub = summary.get('submit_latency', {})

    lines = [
        ('ENGINE', PALETTE['accent'], True),
        (f"  mode           {config.get('mode', '?')}", PALETTE['text'], False),
        (f"  workers        {config.get('workers', '?')}", PALETTE['text'], False),
        (f"  instruments    {len(instruments)}", PALETTE['text'], False),
        ('', None, False),
        ('VOLUME', PALETTE['accent'], True),
        (f"  commands       {fmt_num(summary.get('total_commands', 0))}", PALETTE['text'], False),
        (f"  events         {fmt_num(summary.get('total_events', 0))}", PALETTE['text'], False),
        (f"  volume (qty)   {fmt_num(summary.get('volume', 0))}", PALETTE['text'], False),
        (f"  turnover       {fmt_inr(summary.get('turnover', 0))}", PALETTE['green'], False),
        ('', None, False),
        ('PERFORMANCE', PALETTE['accent'], True),
        (f"  sim time       {summary.get('simulation_time_ms', 0):.1f} ms", PALETTE['text'], False),
        (f"  throughput     {fmt_num(summary.get('throughput_cmds_per_sec', 0))} cmd/s",
         PALETTE['green'], False),
        ('', None, False),
        ('SUBMIT LATENCY', PALETTE['accent'], True),
        (f"  mean           {fmt_ns(sub.get('mean_ns', 0))}", PALETTE['text'], False),
        (f"  p50            {fmt_ns(sub.get('p50_ns', 0))}", PALETTE['green'], False),
        (f"  p95            {fmt_ns(sub.get('p95_ns', 0))}", PALETTE['yellow'], False),
        (f"  p99            {fmt_ns(sub.get('p99_ns', 0))}", PALETTE['orange'], False),
        (f"  p99.9          {fmt_ns(sub.get('p999_ns', 0))}", PALETTE['red'], False),
        (f"  max            {fmt_ns(sub.get('max_ns', 0))}", PALETTE['red'], False),
    ]

    y = 0.98
    dy = 0.045
    for text, color, bold in lines:
        if text:
            ax.text(0.02, y, text,
                    transform=ax.transAxes,
                    color=color,
                    fontsize=9 if bold else 8,
                    fontweight='bold' if bold else 'normal',
                    family='monospace',
                    verticalalignment='top')
        y -= dy


# ─── Main dashboard assembly ────────────────────────────────────────

def build_dashboard(data, out_path):
    summary = data.get('summary', {})
    commands = data.get('commands', [])
    timeline = data.get('throughput_timeline', [])
    snapshots = data.get('book_snapshots', [])
    instruments = data.get('instruments', [])
    config = data.get('config', {})

    fig = plt.figure(figsize=(20, 16))
    fig.patch.set_facecolor(PALETTE['bg'])

    # Title
    fig.text(0.5, 0.978,
             'ORDER MATCHING ENGINE · PERFORMANCE DASHBOARD',
             ha='center', va='top', color=PALETTE['accent'],
             fontsize=18, fontweight='bold', family='monospace')
    fig.text(0.5, 0.958,
             f"generated {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}  ·  "
             f"{len(commands):,} commands  ·  {len(instruments)} instruments  ·  "
             f"{summary.get('simulation_time_ms', 0):.0f}ms sim time  ·  "
             f"{fmt_num(summary.get('throughput_cmds_per_sec', 0))} cmd/s",
             ha='center', va='top', color=PALETTE['text_dim'],
             fontsize=9, family='monospace')

    gs = GridSpec(5, 4, figure=fig,
                   left=0.05, right=0.98, top=0.93, bottom=0.04,
                   hspace=0.65, wspace=0.32)

    # Row 0 + 1: summary text (col 0, spans 2 rows) + throughput (cols 1-3, row 0) + scatter (cols 1-3, row 1)
    ax_summary = fig.add_subplot(gs[0:2, 0])
    panel_summary_text(ax_summary, summary, config, instruments)

    ax_tp = fig.add_subplot(gs[0, 1:])
    panel_throughput_timeline(ax_tp, timeline)

    ax_scatter = fig.add_subplot(gs[1, 1:])
    panel_latency_scatter(ax_scatter, commands)

    # Row 2: percentiles (cols 0-1) + histogram (col 2) + CDF (col 3)
    ax_pct = fig.add_subplot(gs[2, 0:2])
    panel_latency_percentiles(ax_pct, summary)

    ax_hist = fig.add_subplot(gs[2, 2])
    panel_latency_histogram(ax_hist, commands)

    ax_cdf = fig.add_subplot(gs[2, 3])
    panel_latency_cdf(ax_cdf, commands)

    # Row 3: command-type latency (col 0) + book spread (cols 1-2) + event breakdown (col 3)
    ax_cmd = fig.add_subplot(gs[3, 0])
    panel_command_type_latency(ax_cmd, commands)

    ax_book = fig.add_subplot(gs[3, 1:3])
    panel_book_spread(ax_book, snapshots, config.get('watch_instrument'))

    ax_evt = fig.add_subplot(gs[3, 3])
    panel_event_breakdown(ax_evt, summary)

    # Row 4: instrument activity (full width)
    ax_inst = fig.add_subplot(gs[4, :])
    panel_instrument_activity(ax_inst, commands)

    fig.savefig(out_path, dpi=120, facecolor=PALETTE['bg'])
    plt.close(fig)
    print(f"wrote {out_path}")


# ─── HTML report generator ──────────────────────────────────────────

def build_html_report(data, out_path, dashboard_img='dashboard.png'):
    summary = data.get('summary', {})
    config = data.get('config', {})
    instruments = data.get('instruments', [])
    sub = summary.get('submit_latency', {})
    e2e = summary.get('e2e_latency', {})

    def row(label, value, cls=''):
        return f'<tr><td class="label">{label}</td><td class="val {cls}">{value}</td></tr>'

    html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Order Matching Engine — Simulation Report</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;600&family=Outfit:wght@400;700;900&display=swap');

  :root {{
    --bg: #0a0e17;
    --panel: #0f1520;
    --grid: #1a2332;
    --text: #c8d6e5;
    --text-dim: #546e7a;
    --accent: #e8eaf6;
    --green: #00e676;
    --red: #ef5350;
    --yellow: #ffa726;
    --blue: #42a5f5;
  }}

  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: 'JetBrains Mono', monospace;
    background: var(--bg);
    color: var(--text);
    padding: 40px 20px;
    line-height: 1.6;
  }}
  .container {{ max-width: 1400px; margin: 0 auto; }}

  header {{ margin-bottom: 30px; }}
  h1 {{
    font-family: 'Outfit', sans-serif;
    font-weight: 900;
    font-size: 32px;
    letter-spacing: 2px;
    color: var(--accent);
    margin-bottom: 6px;
  }}
  .subtitle {{ color: var(--text-dim); font-size: 13px; }}

  .stats-grid {{
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 16px;
    margin: 30px 0;
  }}
  .stat {{
    background: var(--panel);
    border: 1px solid var(--grid);
    border-radius: 6px;
    padding: 18px;
  }}
  .stat-label {{
    font-size: 10px;
    color: var(--text-dim);
    text-transform: uppercase;
    letter-spacing: 1.5px;
    margin-bottom: 8px;
  }}
  .stat-value {{
    font-family: 'Outfit', sans-serif;
    font-size: 28px;
    font-weight: 700;
    color: var(--accent);
  }}
  .stat-value.green {{ color: var(--green); }}
  .stat-value.yellow {{ color: var(--yellow); }}
  .stat-sub {{
    font-size: 10px;
    color: var(--text-dim);
    margin-top: 4px;
  }}

  .section {{
    background: var(--panel);
    border: 1px solid var(--grid);
    border-radius: 6px;
    padding: 20px;
    margin-bottom: 20px;
  }}
  .section-title {{
    font-family: 'Outfit', sans-serif;
    font-weight: 700;
    font-size: 12px;
    color: var(--text-dim);
    letter-spacing: 2px;
    margin-bottom: 14px;
    border-bottom: 1px solid var(--grid);
    padding-bottom: 8px;
  }}

  .dashboard-img {{
    width: 100%;
    border: 1px solid var(--grid);
    border-radius: 6px;
    display: block;
  }}

  table {{
    width: 100%;
    font-size: 12px;
    border-collapse: collapse;
  }}
  td {{ padding: 6px 12px; border-bottom: 1px solid var(--grid); }}
  td.label {{ color: var(--text-dim); width: 40%; }}
  td.val {{ color: var(--text); font-weight: 600; text-align: right; }}
  td.val.green {{ color: var(--green); }}
  td.val.yellow {{ color: var(--yellow); }}
  td.val.red {{ color: var(--red); }}

  .two-col {{
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
  }}

  footer {{
    text-align: center;
    color: var(--text-dim);
    font-size: 10px;
    margin-top: 40px;
    padding-top: 20px;
    border-top: 1px solid var(--grid);
  }}
</style>
</head>
<body>
<div class="container">

<header>
  <h1>ORDER MATCHING ENGINE</h1>
  <div class="subtitle">PERFORMANCE BENCHMARK REPORT · generated {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</div>
</header>

<div class="stats-grid">
  <div class="stat">
    <div class="stat-label">THROUGHPUT</div>
    <div class="stat-value green">{fmt_num(summary.get('throughput_cmds_per_sec', 0))}</div>
    <div class="stat-sub">commands / second</div>
  </div>
  <div class="stat">
    <div class="stat-label">SUBMIT P50</div>
    <div class="stat-value">{fmt_ns(sub.get('p50_ns', 0))}</div>
    <div class="stat-sub">gateway enqueue median</div>
  </div>
  <div class="stat">
    <div class="stat-label">SUBMIT P99</div>
    <div class="stat-value yellow">{fmt_ns(sub.get('p99_ns', 0))}</div>
    <div class="stat-sub">tail latency</div>
  </div>
  <div class="stat">
    <div class="stat-label">TURNOVER</div>
    <div class="stat-value">{fmt_inr(summary.get('turnover', 0))}</div>
    <div class="stat-sub">notional traded</div>
  </div>
</div>

<div class="section">
  <div class="section-title">PERFORMANCE DASHBOARD</div>
  <img class="dashboard-img" src="{dashboard_img}" alt="dashboard">
</div>

<div class="two-col">

  <div class="section">
    <div class="section-title">SUBMIT LATENCY (GATEWAY ENQUEUE)</div>
    <table>
      {row('count',      fmt_num(sub.get('count', 0)))}
      {row('mean',       fmt_ns(sub.get('mean_ns', 0)))}
      {row('std dev',    fmt_ns(sub.get('stddev_ns', 0)))}
      {row('p50',        fmt_ns(sub.get('p50_ns', 0)), 'green')}
      {row('p90',        fmt_ns(sub.get('p90_ns', 0)))}
      {row('p95',        fmt_ns(sub.get('p95_ns', 0)), 'yellow')}
      {row('p99',        fmt_ns(sub.get('p99_ns', 0)), 'yellow')}
      {row('p99.9',      fmt_ns(sub.get('p999_ns', 0)), 'red')}
      {row('min',        fmt_ns(sub.get('min_ns', 0)))}
      {row('max',        fmt_ns(sub.get('max_ns', 0)), 'red')}
    </table>
  </div>

  <div class="section">
    <div class="section-title">END-TO-END LATENCY</div>
    <table>
      {row('count',      fmt_num(e2e.get('count', 0)))}
      {row('mean',       fmt_ns(e2e.get('mean_ns', 0)))}
      {row('std dev',    fmt_ns(e2e.get('stddev_ns', 0)))}
      {row('p50',        fmt_ns(e2e.get('p50_ns', 0)), 'green')}
      {row('p90',        fmt_ns(e2e.get('p90_ns', 0)))}
      {row('p95',        fmt_ns(e2e.get('p95_ns', 0)), 'yellow')}
      {row('p99',        fmt_ns(e2e.get('p99_ns', 0)), 'yellow')}
      {row('p99.9',      fmt_ns(e2e.get('p999_ns', 0)), 'red')}
      {row('min',        fmt_ns(e2e.get('min_ns', 0)))}
      {row('max',        fmt_ns(e2e.get('max_ns', 0)), 'red')}
    </table>
  </div>

  <div class="section">
    <div class="section-title">EVENT BREAKDOWN</div>
    <table>
      {row('total events',  fmt_num(summary.get('total_events', 0)))}
      {row('fills',         fmt_num(summary.get('fills', 0)), 'green')}
      {row('partial fills', fmt_num(summary.get('partial_fills', 0)), 'green')}
      {row('accepted',      fmt_num(summary.get('accepted', 0)))}
      {row('cancelled',     fmt_num(summary.get('cancelled', 0)), 'yellow')}
      {row('rejected',      fmt_num(summary.get('rejected', 0)), 'red')}
      {row('volume (qty)',  fmt_num(summary.get('volume', 0)))}
      {row('turnover',      fmt_inr(summary.get('turnover', 0)), 'green')}
    </table>
  </div>

  <div class="section">
    <div class="section-title">CONFIGURATION</div>
    <table>
      {row('mode',              config.get('mode', '?'))}
      {row('workers',           config.get('workers', '?'))}
      {row('snapshot interval', f"{config.get('snapshot_ms', '?')} ms")}
      {row('depth levels',      config.get('depth_levels', '?'))}
      {row('instruments',       len(instruments))}
      {row('total commands',    fmt_num(summary.get('total_commands', 0)))}
      {row('sim time',          f"{summary.get('simulation_time_ms', 0):.1f} ms")}
    </table>
  </div>

</div>

<footer>
  metrics measured directly from engine::MatchingCore hot path<br>
  generated by visualize.py
</footer>

</div>
</body>
</html>
"""
    with open(out_path, 'w') as f:
        f.write(html)
    print(f"wrote {out_path}")


def main():
    p = argparse.ArgumentParser(description='Visualize sim_runner results')
    p.add_argument('--input', '-i', default='sim_results.json',
                   help='input JSON from sim_runner')
    p.add_argument('--output-dir', '-o', default='./report',
                   help='output directory for dashboard.png and report.html')
    p.add_argument('--dashboard-name', '-d', default='dashboard.png',
                   help='name of the dashboard image inside the output dir')
    p.add_argument('--report-name', '-r', default='report.html',
                   help='name of the HTML report inside the output dir')
    args = p.parse_args()

    if not os.path.exists(args.input):
        print(f"error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output_dir, exist_ok=True)
    with open(args.input) as f:
        data = json.load(f)

    setup_style()
    dashboard_path = os.path.join(args.output_dir, args.dashboard_name)
    build_dashboard(data, dashboard_path)

    report_path = os.path.join(args.output_dir, args.report_name)
    build_html_report(data, report_path, dashboard_img=args.dashboard_name)

    print(f"\nopen {report_path} in a browser to view the full report")


if __name__ == '__main__':
    main()