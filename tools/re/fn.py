"""Print Ghidra decompilation of functions: python fn.py 14093fe30 [1402ee280 ...] (addresses in hex, no 0x)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_paths import DECOMP, rows

idx = {p[0]: p[2] for p in rows(os.path.join(DECOMP, 'index.tsv'), 'entry')}


def get(a):
    a = a.lower().replace('0x', '')
    sh = idx.get(a)
    if not sh:
        return f'// {a}: not in index (not a function start?)'
    out = []; on = False
    with open(os.path.join(DECOMP, sh), encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('//==== '):
                if on: break
                on = line.split()[1] == a
            if on: out.append(line.rstrip('\r\n'))
    return '\n'.join(out)


if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(get(a))
