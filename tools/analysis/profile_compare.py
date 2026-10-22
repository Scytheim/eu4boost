"""Main-thread share of key functions/modules across several profiles:
python profile_compare.py a.tsv b.tsv [...]"""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof_common import load, fn, main_thread_index

KEYS = {'daily tick 140758430': 0x140758430, 'scene update 1414c5810': 0x1414c5810, 'render 1408183b0': 0x1408183b0,
        'POPS wrapper 1414ada90': 0x1414ada90, 'Nakama tick 14168dff0': 0x14168dff0,
        'RefreshKnowledge 14093fe30': 0x14093fe30, 'parallel wait 140179e90': 0x140179e90,
        'country daily 1402ee280': 0x1402ee280, 'monthly serial 1402f36c0': 0x1402f36c0}
MODS = ['pops_api.dll', 'nakama-cpp.dll', 'eu4boost.dll', 'ntdll.dll', 'KERNELBASE.dll', 'nvd3dumx.dll', 'd3d9.dll']
res = []
for p in sys.argv[1:]:
    meta, mods, threads, stacks = load(p); mi = main_thread_index(meta, threads)
    tot = 0; inc = collections.Counter(); modinc = collections.Counter(); modself = collections.Counter()
    for ti, w, fr in stacks:
        if ti != mi: continue
        tot += w; seen = set(); mseen = set()
        for k, (m, o) in enumerate(fr):
            f = fn(m, o, mods); mn = mods.get(m, '?')
            if k == 0: modself[mn] += w
            if f not in seen: seen.add(f); inc[f] += w
            if mn not in mseen: mseen.add(mn); modinc[mn] += w
    res.append((os.path.basename(p), tot, inc, modinc, modself))
print(' ' * 34 + ''.join(f"{r[0][-12:]:>13s}" for r in res))
for lab, a in KEYS.items(): print(f"{lab:34s}" + ''.join(f"{100 * r[2][a] / r[1]:12.2f}%" for r in res))
for m in MODS: print(f"{'incl ' + m:34s}" + ''.join(f"{100 * r[3][m] / r[1]:12.2f}%" for r in res))
for m in ('eu4boost.dll', 'ntdll.dll'): print(f"{'self ' + m:34s}" + ''.join(f"{100 * r[4][m] / r[1]:12.2f}%" for r in res))
