"""Per parallel-for loop efficiency from a profile (main thread + workers):
python profile_parallel.py eu4boost_profile_X.tsv
Columns are % of main-thread time: main wall in the loop (wait+dispatch), spin, item work by main and
by workers, speedup = total work / main wall, ideal12 = total work / 12."""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof_common import load, fn, main_thread_index

WAIT, DISP, WBODY = 0x140179e90, 0x1401798a0, 0x14017a200
prof = sys.argv[1]
meta, mods, threads, stacks = load(prof)
mi = main_thread_index(meta, threads)
tot = 0; lam2loop = {}; M = collections.defaultdict(collections.Counter)
for ti, w, fr in stacks:
    if ti != mi: continue
    fs = [fn(m, o, mods) for m, o in fr]; tot += w
    if WAIT in fs:
        i = fs.index(WAIT); loop = fs[i + 1] if i + 1 < len(fs) else None
        M[loop]['incl'] += w
        if i == 0: M[loop]['spin'] += w
        else: lam2loop.setdefault(fs[i - 1], loop); M[loop]['mainwork'] += w
    if DISP in fs:
        i = fs.index(DISP); M[fs[i + 1] if i + 1 < len(fs) else None]['disp'] += w
for ti, w, fr in stacks:
    if ti == mi: continue
    fs = [fn(m, o, mods) for m, o in fr]
    if WBODY in fs:
        i = fs.index(WBODY)
        if i > 0 and fs[i - 1] in lam2loop: M[lam2loop[fs[i - 1]]]['workerwork'] += w
print(f"main-thread samples {tot}")
print(f"{'loop fn':12s} {'main wall':>9s} {'spin':>6s} {'disp':>6s} {'mainwork':>8s} {'wkwork':>8s} {'speedup':>8s} {'ideal12':>8s}")
for loop, c in sorted(M.items(), key=lambda kv: -(kv[1]['incl'] + kv[1]['disp'])):
    work = c['mainwork'] + c['workerwork']; wall = c['incl'] + c['disp']
    name = f"{loop:x}" if isinstance(loop, int) else str(loop)
    print(f"{name:12s} {100 * wall / tot:9.2f} {100 * c['spin'] / tot:6.2f} {100 * c['disp'] / tot:6.2f} {100 * c['mainwork'] / tot:8.2f} {100 * c['workerwork'] / tot:8.2f} {work / wall if wall else 0:7.1f}x {100 * work / 12 / tot:8.2f}")
