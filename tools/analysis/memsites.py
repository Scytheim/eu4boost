"""Compare the snapshots of plugins\\eu4boost_memsites.tsv ([diag] mem_sites, src/memdiag.c).

python tools/analysis/memsites.py eu4boost_memsites.tsv [--from N] [--to M] [--top 30]

Prints the process totals of every snapshot, then the call sites whose sampled live memory grew the most
between snapshot N (default: the first one with a game date) and M (default: the last one). The numbers
are estimates (samples x the sampling rate). Frames are eu4+RVA (add 0x140000000 for Ghidra).
"""
import argparse, bisect, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))

_fn = None
def fname(fr):
    """eu4+RVA -> FUN_xxx+off with the Ghidra export (tools/re/re_paths.py), else the frame as it is."""
    global _fn
    if not fr.startswith('eu4+'):
        return fr
    if _fn is None:
        _fn = ([], [])
        try:
            from re_paths import FUNCTIONS, rows
            for r in sorted(rows(FUNCTIONS, 'entry'), key=lambda r: int(r[0], 16)):
                _fn[0].append(int(r[0], 16)); _fn[1].append((r[1], int(r[3] or 0)))
        except Exception:
            pass
    va = 0x140000000 + int(fr[4:], 16)
    i = bisect.bisect_right(_fn[0], va) - 1
    if i >= 0 and va < _fn[0][i] + max(_fn[1][i][1], 1):
        return f"{_fn[1][i][0]}+{va - _fn[0][i]:x}"
    return fr


def load(path):
    snaps = []
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\r\n').split('\t')
        if f[0] == '#snap':
            kv = dict(zip(f[4::2], f[5::2]))
            snaps.append({'n': int(f[1]), 'time': f[2], 'date': f[3], 'kv': kv, 'sites': {}, 'mem': {}, 'blocks': {}})
        elif f[0] == '#mem' and snaps:
            snaps[-1]['mem'] = dict(zip(f[1::2], f[2::2]))
        elif f[0] == '#block' and snaps:
            snaps[-1]['blocks'][f[1]] = (float(f[2]), float(f[3]))
        elif snaps and len(f) >= 5:
            snaps[-1]['sites'][int(f[0])] = {'live': float(f[1]), 'n': float(f[2]), 'allocs': float(f[3]), 'mb': float(f[4]),
                                             'fr': f[5:]}
    return snaps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tsv'); ap.add_argument('--from', dest='a', type=int); ap.add_argument('--to', dest='b', type=int)
    ap.add_argument('--top', type=int, default=30)
    o = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    snaps = load(o.tsv)
    if not snaps:
        print('no snapshots'); return
    print('snap  time      date        private  ws     live(est)  blocks(est)')
    for s in snaps:
        k = s['kv']
        print(f"{s['n']:4d}  {s['time']}  {s['date']:10s}  {k.get('private_mb', '?'):>7s}  {k.get('ws_mb', '?'):>5s}  {k.get('live_mb_est', '?'):>9s}  {k.get('live_blocks_est', '?'):>11s}")
    if any(s['mem'] for s in snaps):
        print('\nsnap  mimalloc  other_private  heaps_commit  heaps_alloc  mapped')
        for s in snaps:
            m = s['mem']
            if m:
                print(f"{s['n']:4d}  {m['mimalloc_mb']:>8s}  {m['other_private_mb']:>13s}  {m['heaps_commit_mb']:>12s}  {m['heaps_alloc_mb']:>11s}  {m['mapped_mb']:>6s}")
    by_n = {s['n']: s for s in snaps}
    a = by_n.get(o.a) if o.a else next((s for s in snaps if s['date'] != '-'), snaps[0])
    b = by_n.get(o.b) if o.b else snaps[-1]
    print(f"\ngrowth from snapshot {a['n']} ({a['date']}) to {b['n']} ({b['date']}): private "
          f"{int(b['kv']['private_mb']) - int(a['kv']['private_mb']):+d} MB, sampled live {float(b['kv']['live_mb_est']) - float(a['kv']['live_mb_est']):+.0f} MB")
    # a site missing from a snapshot's top list counts as 0 there (only the top 150 are written)
    ids = set(a['sites']) | set(b['sites'])
    rows = []
    for i in ids:
        x = a['sites'].get(i, {}).get('live', 0.0); y = b['sites'].get(i, {}).get('live', 0.0)
        fr = (b['sites'].get(i) or a['sites'].get(i))['fr']
        rows.append((y - x, x, y, i, fr, b['sites'].get(i, {}).get('n', 0.0)))
    rows.sort(reverse=True)
    print(f"\n{'grow MB':>8s} {'from':>8s} {'to':>8s} {'blocks':>9s}  site  frames")
    for d, x, y, i, fr, n in rows[:o.top]:
        print(f"{d:8.1f} {x:8.1f} {y:8.1f} {n:9.0f}  {i:5d}  {' < '.join(fname(f) for f in fr if f != '-')}")
    if a['blocks'] or b['blocks']:
        print('\nbig blocks outside mimalloc (committed MB):')
        for base in sorted(set(a['blocks']) | set(b['blocks']), key=lambda k: -b['blocks'].get(k, (0, 0))[0]):
            x = a['blocks'].get(base, (0, 0))[0]; y = b['blocks'].get(base, (0, 0))[0]
            print(f"  {base:>14s}  {x:8.1f} -> {y:8.1f}  ({y - x:+.1f})")
    print('\nlargest shrink:')
    for d, x, y, i, fr, n in sorted(rows)[:5]:
        print(f"{d:8.1f} {x:8.1f} {y:8.1f} {n:9.0f}  {i:5d}  {' < '.join(fname(f) for f in fr if f != '-')}")


if __name__ == '__main__':
    main()
