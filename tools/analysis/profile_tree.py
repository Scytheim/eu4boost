"""Inclusive call tree on the main thread under a root function:
python profile_tree.py eu4boost_profile_X.tsv 140758430 [depth=3] [min_percent=1.0]
Useful roots: 140758430 daily tick, 14075b0d0 country serial, 1402ee280 CCountry daily update, 14075d690."""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof_common import load, fn, label, main_thread_index

prof = sys.argv[1]; root = int(sys.argv[2], 16)
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 3
thr = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
meta, mods, threads, stacks = load(prof)
mi = main_thread_index(meta, threads)
tot = 0; tree = collections.Counter()
for ti, w, fr in stacks:
    if ti != mi: continue
    tot += w
    fs = [fn(m, o, mods) for m, o in fr][::-1]
    if root not in fs: continue
    path = []
    for f in fs[fs.index(root):]:
        if not path or path[-1] != f: path.append(f)
    for d in range(1, min(depth + 1, len(path)) + 1): tree[tuple(path[:d])] += w


def show(prefix, ind):
    kids = sorted(((k, n) for k, n in tree.items() if len(k) == len(prefix) + 1 and k[:len(prefix)] == prefix), key=lambda x: -x[1])
    for k, n in kids:
        if 100 * n / tot < thr: continue
        print('  ' * ind + f"{100 * n / tot:5.1f}% {label(k[-1])}")
        if len(k) < depth + 1: show(k, ind + 1)


print(f"{100 * tree[(root,)] / tot:.1f}% {label(root)}  (main-thread samples {tot})")
show((root,), 1)
