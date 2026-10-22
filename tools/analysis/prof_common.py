"""Load eu4boost_profile_*.tsv (Ctrl+Alt+P sampling profiler) and map frames to eu4.exe functions.

File format (written by src/profiler.c):
  #key<TAB>value        meta (interval_us, seconds, ticks, samples, main_tid ...)
  M idx base size name  loaded module
  T idx tid samples start  thread (start = module+offset of thread start address)
  S thread_idx count stack   stack = "mod:offset;mod:offset;..." leaf first, offsets hex
"""
import bisect, collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
from re_paths import FUNCTIONS, STRING_REFS, IMAGE_BASE, rows

_ents = None; _strs = None


def _load_symbols():
    global _ents, _strs
    if _ents is None:
        _ents = sorted(int(p[0], 16) for p in rows(FUNCTIONS, 'entry'))
        _strs = collections.defaultdict(list)
        for p in rows(STRING_REFS, 'function'):
            if 'mnt' not in p[2] and len(p[2]) > 3: _strs[int(p[0], 16)].append(p[2])


def load(path):
    mods = {}; threads = {}; stacks = []; meta = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        p = line.rstrip('\r\n').split('\t')
        if p[0].startswith('#'): meta[p[0][1:]] = p[1] if len(p) > 1 else ''
        elif p[0] == 'M': mods[int(p[1])] = p[4]
        elif p[0] == 'T': threads[int(p[1])] = (p[2], p[4] if len(p) > 4 else '')
        elif p[0] == 'S':
            fr = []
            for x in p[3].split(';'):
                m, _, o = x.partition(':'); fr.append((int(m) if m != '?' else -1, int(o, 16)))
            stacks.append((int(p[1]), int(p[2]), fr))
    return meta, mods, threads, stacks


def main_thread_index(meta, threads):
    for i, (tid, _) in threads.items():
        if tid == meta.get('main_tid'): return i
    return None


def fn(m, o, mods):
    """Function entry VA (int) for an eu4.exe frame, else the module name."""
    if mods.get(m) == 'eu4.exe':
        _load_symbols()
        return _ents[bisect.bisect_right(_ents, IMAGE_BASE + o) - 1]
    return mods.get(m, '?')


def label(f):
    if isinstance(f, int):
        _load_symbols()
        s = _strs.get(f, [])
        return f"{f:x}" + (" {" + "|".join(x[:38] for x in s[:2]) + "}" if s else "")
    return f
