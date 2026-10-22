"""Call graph from Ghidra export: python cg.py up 3 14093fe30   (callers, 3 levels)
                                  python cg.py down 1 1402ee280 (direct callees)
Note: calls.tsv 'caller' column is the calling FUNCTION's entry, not the call site."""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_paths import CALLS, FUNCTIONS, STRING_REFS, rows

callers = collections.defaultdict(set); callees = collections.defaultdict(set)
for a, b in rows(CALLS, 'caller'):
    callers[b].add(a); callees[a].add(b)
info = {p[0]: p for p in rows(FUNCTIONS, 'entry')}
strs = collections.defaultdict(list)
for p in rows(STRING_REFS, 'function'): strs[p[0]].append(p[2])


def desc(f, n=12):
    i = info.get(f, ['', '?', '', '0'])
    return f"{f} {i[1]} sz={i[3]} strs={'|'.join(x[:60] for x in strs.get(f, [])[:n])}"


def up(f, depth, ind=0, seen=None):
    seen = seen if seen is not None else set()
    print('  ' * ind + desc(f))
    if depth == 0 or f in seen: return
    seen.add(f)
    cs = sorted(callers.get(f, []))
    if len(cs) > 8: print('  ' * (ind + 1) + f"... {len(cs)} callers: " + ' '.join(cs[:8])); cs = cs[:4]
    for c in cs: up(c, depth - 1, ind + 1, seen)


if __name__ == '__main__':
    mode, depth = sys.argv[1], int(sys.argv[2])
    for f in sys.argv[3:]:
        if mode == 'up': up(f, depth)
        else:
            print(desc(f))
            for c in sorted(callees.get(f, [])): print('   ->', desc(c, 6))
        print('======')
