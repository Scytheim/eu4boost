"""Compare per-loop parallel stats ("parallel: site ..." lines, [parallel] stats=1) between two eu4boost logs:
python parallel_stats.py eu4boost_prev.log eu4boost.log   (e.g. A/B pair from ab_test=1)
Per call: wall = dispatch start -> wait end on the main thread. Sites are dispatcher return addresses (RVA)."""
import re, sys

PAT = re.compile(r'site eu4\+([0-9a-f]+) calls (\d+) items/call (\d+) \| per call: wall (\d+) us, dispatch (\d+) us, main wait (\d+) us \| tasks/call ([\d.]+) late (\d+)%')


def parse(p):
    out = {}; variant = ''
    for line in open(p, encoding='utf-8', errors='replace'):
        if 'A/B test' in line: variant = line.split('->')[-1].strip()
        m = PAT.search(line)
        if m:
            s, calls, items, wall, disp, wait, tasks, late = m.groups()
            out[s] = dict(calls=int(calls), items=int(items), wall=int(wall), disp=int(disp), wait=int(wait), tasks=float(tasks), late=int(late))
    return variant, out


va, A = parse(sys.argv[1]); vb, B = parse(sys.argv[2])
print(f"A = {sys.argv[1]} {va}\nB = {sys.argv[2]} {vb}")
print(f"{'site':8s} {'items':>6s} {'A calls':>8s} {'B calls':>8s} {'A us':>7s} {'B us':>7s} {'change':>7s} {'A disp':>6s} {'B disp':>6s} {'tasks A/B':>9s}  saved per call")
for s in sorted(set(A) | set(B), key=lambda s: -(A.get(s, {}).get('wall', 0) * A.get(s, {}).get('calls', 0))):
    a = A.get(s); b = B.get(s)
    if not a or not b: print(f"{s:8s} only in {'A' if a else 'B'}"); continue
    print(f"{s:8s} {a['items']:6d} {a['calls']:8d} {b['calls']:8d} {a['wall']:7d} {b['wall']:7d} {100 * (b['wall'] / a['wall'] - 1) if a['wall'] else 0:+6.0f}% {a['disp']:6d} {b['disp']:6d} {a['tasks']:4.0f}/{b['tasks']:<4.0f}  {(a['wall'] - b['wall']):+7d} us")
