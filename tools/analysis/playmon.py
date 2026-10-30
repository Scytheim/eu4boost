"""Summary of a play monitor recording ([diag] play_monitor=1, src/playmon.c):
python playmon.py eu4boost_play_<start>.csv [step_minutes=30]
Prints the session, then one line per step: game date, memory (private, working set, where it is), frames,
hitches, day times, handles/GDI/threads, the GUI's windows and update time per frame, frame time percentiles
(fr: frames without a daily tick, fd: with one) and the input delay (count, 50th, 90th percentile ms) (since 10-30) and what
the window and the game were doing; then the growth per
hour of play and per game year, and the events file next to the CSV."""
import csv, os, sys

path = sys.argv[1]
step = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
rows = list(csv.DictReader(open(path, encoding='utf-8', errors='replace')))
if not rows:
    sys.exit('no rows')


def f(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError):
        return 0.0


print(f'{len(rows)} rows, {rows[0]["time"]} -> {rows[-1]["time"]}, game {rows[0]["date"]} -> {rows[-1]["date"]}')
print(f'{"time":>8} {"date":>10} {"days":>5} {"run/pau/menu/load":>17} {"fg/bg/min":>11} {"fps":>5} {"h250":>5} {"h1s":>4} {"maxfr":>6} '
      f'{"day avg/max":>12} {"priv":>6} {"ws":>6} {"mimal":>6} {"heaps":>6} {"other":>6} {"ramfree":>7} {"hnd":>6} {"gdi":>5} {"thr":>4} {"cpu":>5} {"main":>5}'
      + (f' {"guiwin":>7} {"gui_ms":>6}' if 'gui_ms' in rows[0] else '')
      + (f' {"fr50/99":>11} {"fd50":>5} {"in n/50/90":>13}' if 'fr_p50' in rows[0] else ''))
acc = None
last_t = None


def flush(a, r):
    up = f(r, 'uptime_s') / 3600
    sec = max(a['n'], 1)
    print(f'{up:7.2f}h {r["date"]:>10} {a["days"]:5.0f} {a["run"]:4.0f}/{a["pau"]:4.0f}/{a["menu"]:4.0f}/{a["load"]:3.0f} '
          f'{a["fg"]:3.0f}/{a["bg"]:3.0f}/{a["min"]:3.0f} {a["frames"] / max(a["secs"], 1):5.1f} {a["h250"]:5.0f} {a["h1s"]:4.0f} {a["maxfr"]:6.0f} '
          f'{(a["dsum"] / a["dn"] if a["dn"] else 0):5.1f}/{a["dmax"]:6.0f} {f(r, "private_mb"):6.0f} {f(r, "working_set_mb"):6.0f} '
          f'{f(r, "mimalloc_mb"):6.0f} {f(r, "heaps_commit_mb"):6.0f} {f(r, "other_private_mb"):6.0f} {f(r, "ram_free_mb"):7.0f} '
          f'{f(r, "handles"):6.0f} {f(r, "gdi"):5.0f} {f(r, "threads"):4.0f} {a["cpu"] / sec:5.1f} {a["main"] / sec:5.1f}'
          + (f' {f(r, "gui_windows"):7.0f} {a["gui"] / a["frames"] if a["frames"] else 0:6.2f}' if 'gui_ms' in r else '')
          + (f' {a["fr50"] / a["n"]:5.1f}/{a["fr99"] / a["n"]:5.0f} {a["fd50"] / a["n"]:5.0f} {a["inn"]:4.0f}/{(a["in50"] / a["inw"]) if a["inw"] else 0:4.0f}/{(a["in90"] / a["inw"]) if a["inw"] else 0:4.0f}'
             if 'fr_p50' in r else ''))


for r in rows:
    t = f(r, 'uptime_s')
    if acc is None:
        acc = dict(t0=t, n=0, days=0, run=0, pau=0, menu=0, load=0, fg=0, bg=0, min=0, frames=0, secs=0, h250=0, h1s=0, maxfr=0, dsum=0, dn=0, dmax=0, cpu=0, main=0, gui=0, fr50=0, fr99=0, fd50=0, inn=0, in50=0, in90=0, inw=0)
    a = acc
    a['n'] += 1; a['days'] += f(r, 'days')
    a['run'] += f(r, 'run_s'); a['pau'] += f(r, 'paused_s'); a['menu'] += f(r, 'menu_s'); a['load'] += f(r, 'loading_s')
    a['fg'] += f(r, 'front_s'); a['bg'] += f(r, 'behind_s'); a['min'] += f(r, 'minimized_s')
    a['frames'] += f(r, 'frames'); a['secs'] += f(r, 'run_s') + f(r, 'paused_s') + f(r, 'menu_s') + f(r, 'loading_s')
    a['h250'] += f(r, 'frames_250ms'); a['h1s'] += f(r, 'frames_1s'); a['maxfr'] = max(a['maxfr'], f(r, 'longest_frame_ms'))
    a['dsum'] += f(r, 'day_avg_ms') * f(r, 'days_ticked'); a['dn'] += f(r, 'days_ticked'); a['dmax'] = max(a['dmax'], f(r, 'day_max_ms'))
    a['cpu'] += f(r, 'cpu_pct'); a['main'] += f(r, 'main_cpu_pct'); a['gui'] += f(r, 'gui_ms') * f(r, 'frames')
    a['fr50'] += f(r, 'fr_p50'); a['fr99'] += f(r, 'fr_p99'); a['fd50'] += f(r, 'fd_p50')
    a['inn'] += f(r, 'inputs'); a['in50'] += f(r, 'in_p50') * f(r, 'inputs'); a['in90'] += f(r, 'in_p90') * f(r, 'inputs'); a['inw'] += f(r, 'inputs')
    if t - a['t0'] >= step * 60:
        flush(a, r); acc = None
if acc is not None:
    flush(acc, rows[-1])

# growth: from the first row after the latest load (date jump) to the last, per hour and per game year
start = max([i for i, r in enumerate(rows) if r.get('date_jump') == '1'] + [0])
game = [r for r in rows[start:] if r['date'] != '-']
if len(game) >= 2:
    a, b = game[0], game[-1]
    hrs = (f(b, 'uptime_s') - f(a, 'uptime_s')) / 3600
    years = sum(f(r, 'days') for r in game[1:]) / 365
    print()
    for k in ('private_mb', 'working_set_mb', 'mimalloc_mb', 'heaps_commit_mb', 'other_private_mb', 'mapped_mb', 'handles', 'gdi', 'user', 'threads'):
        d = f(b, k) - f(a, k)
        print(f'{k:>18}: {f(a, k):8.0f} -> {f(b, k):8.0f} ({d:+8.0f}; {d / hrs if hrs > 0.05 else 0:+8.1f} per hour, '
              f'{d / years if years > 0.05 else 0:+8.1f} per game year; max {max(f(r, k) for r in game):8.0f})')
    print(f'since the latest load: {hrs:.1f} h of play, {years:.1f} game years (loads in the file: {sum(int(f(r, "date_jump")) for r in rows)})')
    print(f'frames over 250 ms {sum(f(r, "frames_250ms") for r in rows):.0f}, over 1 s {sum(f(r, "frames_1s") for r in rows):.0f}, '
          f'longest {max(f(r, "longest_frame_ms") for r in rows):.0f} ms; longest day {max(f(r, "day_max_ms") for r in rows):.0f} ms')

ev = path[:-4] + '_events.txt'
if os.path.exists(ev):
    lines = open(ev, encoding='utf-8', errors='replace').read().splitlines()
    kinds = {}
    for l in lines:
        k = l[30:].split(':')[0].split(' (')[0].strip() if len(l) > 30 else l
        kinds[k] = kinds.get(k, 0) + 1
    print(f'\nevents: {len(lines)} - ' + ', '.join(f'{k} {v}' for k, v in sorted(kinds.items(), key=lambda x: -x[1])[:15]))
    for l in lines:
        if any(s in l for s in ('memory', 'handles', 'GDI', 'threads', 'slow report', 'hang', 'long', 'low', 'load', 'start')):
            print('  ' + l[:200])
