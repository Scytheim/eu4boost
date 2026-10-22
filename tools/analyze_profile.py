#!/usr/bin/env python3
"""Summarise an eu4boost sampling profile.

usage: analyze_profile.py profile.tsv [--functions functions.tsv] [--strings string_refs.tsv]
                          [--top 40] [--collapsed out.folded] [--thread N]

* resolves eu4.exe frames to Ghidra function entries (functions.tsv from ExportInventory,
  image base 0x140000000) and labels them with a few referenced strings
* prints, per busy thread: share of samples, top self functions, top inclusive functions
* --collapsed writes Brendan-Gregg folded stacks (for flamegraph.pl / speedscope)
"""
import argparse, bisect, collections, os, sys

GHIDRA_BASE = 0x140000000


def load_functions(path):
    if not path or not os.path.exists(path):
        return [], {}
    ents, names = [], {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\r\n').split('\t')
            if p[0] == 'entry' or len(p) < 4:
                continue
            try:
                a = int(p[0], 16)
            except ValueError:
                continue
            ents.append(a)
            names[a] = (p[1], int(p[3]) if p[3].isdigit() else 0)
    ents.sort()
    return ents, names


def load_strings(path):
    out = collections.defaultdict(list)
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\r\n').split('\t')
            if p[0] == 'function' or len(p) < 3:
                continue
            s = p[2]
            if 'C:\\\\mnt' in s or len(s) < 4:
                continue
            out[p[0]].append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--functions')
    ap.add_argument('--strings')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--collapsed')
    ap.add_argument('--thread', type=int, action='append')
    a = ap.parse_args()

    ents, fnames = load_functions(a.functions)
    fstrings = load_strings(a.strings)

    meta, mods, threads, stacks = {}, {}, {}, []
    with open(a.profile, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\r\n').split('\t')
            if p[0].startswith('#'):
                meta[p[0][1:]] = p[1] if len(p) > 1 else ''
            elif p[0] == 'M':
                mods[int(p[1])] = (int(p[2], 16), int(p[3], 16), p[4])
            elif p[0] == 'T':
                threads[int(p[1])] = dict(tid=int(p[2]), samples=int(p[3]), start=p[4], desc=p[5] if len(p) > 5 else '')
            elif p[0] == 'S':
                frames = []
                for fr in p[3].split(';'):
                    m, _, off = fr.partition(':')
                    frames.append((int(m) if m != '?' else -1, int(off, 16)))
                stacks.append((int(p[1]), int(p[2]), frames))

    exe_idx = next((i for i, m in mods.items() if m[2].lower() == 'eu4.exe'), None)
    cache = {}

    def sym(fr):
        if fr in cache:
            return cache[fr]
        m, off = fr
        if m == exe_idx and ents:
            va = GHIDRA_BASE + off
            i = bisect.bisect_right(ents, va) - 1
            if i >= 0:
                e = ents[i]
                name, size = fnames[e]
                if size == 0 or va < e + max(size, 1) + 0x200:
                    label = name
                    hints = fstrings.get('%x' % e)
                    if hints:
                        label += ' {' + '|'.join(h[:40] for h in hints[:2]) + '}'
                    cache[fr] = label
                    return label
            cache[fr] = 'eu4.exe+%x' % off
        elif m in mods:
            cache[fr] = '%s+%x' % (mods[m][2], off) if m != exe_idx else 'eu4.exe+%x' % off
            # collapse non-eu4 frames to module granularity for readability
            cache[fr] = mods[m][2]
        else:
            cache[fr] = '?'
        return cache[fr]

    total = sum(c for _, c, _ in stacks)
    print('profile: %s' % a.profile)
    for k in ('seconds', 'interval_us', 'ticks', 'samples', 'dropped', 'unwind_fail', 'main_tid'):
        if k in meta:
            print('  %-12s %s' % (k, meta[k]))
    ticks = int(meta.get('ticks', '0') or 0)
    print()
    print('threads (busy share = samples / sampler ticks):')
    per_thread = collections.Counter()
    for t, c, _ in stacks:
        per_thread[t] += c
    for t, c in per_thread.most_common():
        th = threads.get(t, {})
        busy = (100.0 * c / ticks) if ticks else 0
        tag = ' MAIN' if str(th.get('tid')) == meta.get('main_tid') else ''
        print('  #%-3d tid %-6s %6d samples  busy %5.1f%%  start %s%s' % (t, th.get('tid', '?'), c, busy, th.get('start', '?'), tag))

    sel = a.thread or [t for t, _ in per_thread.most_common(6)]
    folded = collections.Counter()
    for t in sel:
        self_c, incl_c = collections.Counter(), collections.Counter()
        n = 0
        for tt, c, frames in stacks:
            if tt != t:
                continue
            n += c
            names = [sym(fr) for fr in frames]
            # self = first eu4 frame if the leaf is in a system module (waits, CRT, ...)
            self_c[names[0]] += c
            for nm in set(names):
                incl_c[nm] += c
        if not n:
            continue
        th = threads.get(t, {})
        print()
        print('=== thread #%d (tid %s, start %s): %d samples' % (t, th.get('tid'), th.get('start'), n))
        print('  -- self (leaf) --')
        for nm, c in self_c.most_common(a.top):
            print('  %6.2f%%  %s' % (100.0 * c / n, nm))
        print('  -- inclusive --')
        for nm, c in incl_c.most_common(a.top):
            print('  %6.2f%%  %s' % (100.0 * c / n, nm))

    if a.collapsed:
        for t, c, frames in stacks:
            names = [sym(fr).replace(';', ',') for fr in reversed(frames)]
            folded['T%d;%s' % (t, ';'.join(names))] += c
        with open(a.collapsed, 'w', encoding='utf-8') as f:
            for k, c in folded.items():
                f.write('%s %d\n' % (k, c))
        print('\nfolded stacks -> %s' % a.collapsed)


if __name__ == '__main__':
    main()
