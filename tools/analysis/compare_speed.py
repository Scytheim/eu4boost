"""Compare eu4boost_speed_*.csv runs over the date range they all share:
python compare_speed.py name1=run1.csv name2=run2.csv [...]
CSV: date,wall_ms,tick_ms per game day (wall = start of this day -> start of next day, includes rendering;
tick = CDailyTickCommand::Execute incl. mid-tick frames). Rows with empty wall_ms are pauses (> 1 s), skipped.
Runs must start from the same save (1596.09.10 in all tests so far). Old 0.2.1 files have every date as
1.01.01 (bug) - they are aligned by row index then. Same-config runs differ by ~2-3%."""
import csv, math, statistics as st, sys


def rd(p): return [(r['date'], float(r['wall_ms']), float(r['tick_ms'])) for r in csv.DictReader(open(p)) if r['wall_ms']]


runs = []
for a in sys.argv[1:]:
    name, _, path = a.partition('=')
    if not path: name, path = a, a
    runs.append((name, rd(path)))
dated = [r for r in runs if r[1] and r[1][0][0] != '1.01.01']
if dated:
    common = set(d for d, _, _ in dated[0][1])
    for _, v in dated[1:]: common &= set(d for d, _, _ in v)
    order = [d for d, _, _ in dated[0][1] if d in common]
    n = len(order)
    for i, (name, v) in enumerate(runs):
        if v and v[0][0] == '1.01.01': runs[i] = (name, [(order[j], *v[j][1:]) for j in range(min(n, len(v)))])
    sel = [(name, [x for x in v if x[0] in common][:n]) for name, v in runs]
else:
    n = min(len(v) for _, v in runs); sel = [(name, v[:n]) for name, v in runs]
n = min(len(v) for _, v in sel)
print(f"{n} common days: {sel[0][1][0][0]} - {sel[0][1][n - 1][0]}")
base = sum(x[1] for x in sel[0][1][:n])
for name, v in sel:
    v = v[:n]; W = sum(x[1] for x in v); T = sum(x[2] for x in v)
    ms = [x[1] for x in v if x[0].endswith(('.01', '.02'))]; nm = [x[1] for x in v if not x[0].endswith(('.01', '.02'))]
    print(f"{name:16s} {n * 1000 / W:6.2f} days/s  wall {W / 1000:6.2f}s ({100 * (W / base - 1):+5.1f}% vs first)  tick {T / n:5.1f} ms  between days {(W - T) / n:4.1f} ms  normal day {st.mean(nm):5.1f}  month start {st.mean(ms) if ms else 0:6.1f}")
if len(sel) >= 2:
    a, b = sel[0][1], sel[-1][1]
    d = [b[i][1] - a[i][1] for i in range(n)]
    print(f"{sel[-1][0]} - {sel[0][0]}: {st.mean(d):+.2f} ms/day +- {st.stdev(d) / math.sqrt(n):.2f} (SE), faster on {sum(1 for x in d if x < 0)}/{n} days")
