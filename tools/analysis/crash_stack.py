"""x64 stack unwinder for an EU4 crash minidump (uses .pdata/.xdata of the module files on disk).
python tools/analysis/crash_stack.py minidump.dmp [all] [--regs]   -> faulting thread (all: every thread; --regs: saved registers per frame)"""
import bisect, os, struct, sys
import pefile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
from re_paths import GAME, FUNCTIONS

path = sys.argv[1]
ALL = 'all' in sys.argv[2:]
d = open(path, 'rb').read()
nstreams, dir_rva = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dir_rva + 12 * i)
    if t: streams.setdefault(t, (size, rva))


def mstr(rva):
    n = struct.unpack_from('<I', d, rva)[0]
    return d[rva + 4: rva + 4 + n].decode('utf-16-le')


mods = []
size, rva = streams[4]
for i in range(struct.unpack_from('<I', d, rva)[0]):
    o = rva + 4 + 108 * i
    base, sz, cks, ts, name_rva = struct.unpack_from('<QIIII', d, o)
    full = mstr(name_rva)
    mods.append((base, sz, ts, os.path.basename(full), full))
mods.sort()
mbases = [m[0] for m in mods]


def modof(a):
    i = bisect.bisect_right(mbases, a) - 1
    if i >= 0 and a < mods[i][0] + mods[i][1]: return mods[i]
    return None


ranges = []
if 5 in streams:
    size, rva = streams[5]
    for i in range(struct.unpack_from('<I', d, rva)[0]):
        start, dsz, mrva = struct.unpack_from('<QII', d, rva + 4 + 16 * i)
        ranges.append((start, dsz, mrva))
if 9 in streams:
    size, rva = streams[9]
    nr, off = struct.unpack_from('<QQ', d, rva)
    for i in range(nr):
        start, dsz = struct.unpack_from('<QQ', d, rva + 16 + 16 * i)
        ranges.append((start, dsz, off)); off += dsz
ranges.sort()
rstarts = [r[0] for r in ranges]


def read(a, n):
    i = bisect.bisect_right(rstarts, a) - 1
    if i < 0: return None
    s, sz, off = ranges[i]
    if a + n > s + sz: return None
    return d[off + a - s: off + a - s + n]


def q(a):
    b = read(a, 8)
    return struct.unpack('<Q', b)[0] if b else None


_pe = {}


def pe_of(m):
    name = m[3].lower()
    if name in _pe: return _pe[name]
    cands = [m[4], os.path.join(GAME, m[3]), os.path.join(GAME, 'plugins', m[3]), os.path.join(r'C:\Windows\System32', m[3])]
    pe = None
    for c in cands:
        if os.path.isfile(c):
            try:
                p = pefile.PE(c, fast_load=True)
                if p.FILE_HEADER.TimeDateStamp == m[2] or name in ('eu4boost.dll',):
                    pe = p; break
            except Exception: pass
    if pe:
        pdir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        raw = pe.get_data(pdir.VirtualAddress, pdir.Size) if pdir.Size else b''
        rf = [struct.unpack_from('<III', raw, k) for k in range(0, len(raw) - 11, 12)]
        pe._rf = rf; pe._rfb = [r[0] for r in rf]
    _pe[name] = pe
    return pe


# Ghidra names for eu4.exe
fentries = []
for line in open(FUNCTIONS, encoding='utf-8', errors='replace'):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'entry': continue
    try: fentries.append((int(p[0], 16) - 0x140000000, p[1]))
    except ValueError: pass
fentries.sort(); fb = [f[0] for f in fentries]


def sym(a):
    m = modof(a)
    if not m: return f'{a:#x}'
    r = a - m[0]
    s = f'{m[3]}+{r:#x}'
    if m[3].lower() == 'eu4.exe':
        i = bisect.bisect_right(fb, r) - 1
        if i >= 0: s += f' [{fentries[i][1]}+{r - fentries[i][0]:#x}]'
    return s


def unwind_step(regs):
    rip = regs[16]
    m = modof(rip)
    pe = pe_of(m) if m else None
    if not pe:
        return None
    r = rip - m[0]
    i = bisect.bisect_right(pe._rfb, r) - 1
    if i < 0 or not (pe._rf[i][0] <= r < pe._rf[i][1]):   # leaf
        ra = q(regs[4])
        if ra is None: return None
        regs[16] = ra; regs[4] += 8
        return regs
    begin, end, ui = pe._rf[i]
    off_in = r - begin
    first = True
    while True:
        hdr = pe.get_data(ui, 4)
        ver_flags, prolog, ncodes, fr = hdr[0], hdr[1], hdr[2], hdr[3]
        flags = ver_flags >> 3
        freg, foff = fr & 15, fr >> 4
        codes = pe.get_data(ui + 4, 2 * ncodes)
        k = 0
        while k < ncodes:
            co, opb = codes[2 * k], codes[2 * k + 1]
            op, info = opb & 15, opb >> 4
            apply = (not first) or co <= off_in
            if op == 0:   # PUSH_NONVOL
                if apply: regs[info] = q(regs[4]); regs[4] += 8
                k += 1
            elif op == 1:   # ALLOC_LARGE
                if info == 0: sz = struct.unpack_from('<H', codes, 2 * k + 2)[0] * 8; k += 2
                else: sz = struct.unpack_from('<I', codes, 2 * k + 2)[0]; k += 3
                if apply: regs[4] += sz
            elif op == 2:   # ALLOC_SMALL
                if apply: regs[4] += info * 8 + 8
                k += 1
            elif op == 3:   # SET_FPREG
                if apply: regs[4] = regs[freg] - foff * 16
                k += 1
            elif op == 4:   # SAVE_NONVOL
                o = struct.unpack_from('<H', codes, 2 * k + 2)[0] * 8
                if apply: regs[info] = q(regs[4] + o)
                k += 2
            elif op == 5:
                o = struct.unpack_from('<I', codes, 2 * k + 2)[0]
                if apply: regs[info] = q(regs[4] + o)
                k += 3
            elif op == 8: k += 2
            elif op == 9: k += 3
            elif op == 10:   # PUSH_MACHFRAME
                if apply:
                    base = regs[4] + (8 if info else 0)
                    regs[16] = q(base); regs[4] = q(base + 24)
                    return regs
                k += 1
            else:
                k += 1
        if flags & 4:   # CHAININFO
            o = ui + 4 + 2 * ((ncodes + 1) & ~1)
            begin, end, ui = struct.unpack('<III', pe.get_data(o, 12))
            first = False
            continue
        break
    ra = q(regs[4])
    if ra is None: return None
    regs[16] = ra; regs[4] += 8
    return regs


def ctx_regs(ctx_rva):
    v = list(struct.unpack_from('<17Q', d, ctx_rva + 0x78))
    return v   # rax rcx rdx rbx rsp rbp rsi rdi r8..r15 rip


SHOWREGS = '--regs' in sys.argv


def walk(regs, n):
    out = []
    for _ in range(n):
        s = sym(regs[16])
        if SHOWREGS:
            s += '   ' + ' '.join(f'{k}={regs[i]:#x}' for k, i in (('rbx', 3), ('rbp', 5), ('rsi', 6), ('rdi', 7), ('r12', 12), ('r13', 13), ('r14', 14), ('r15', 15)) if regs[i] is not None)
        out.append(s)
        regs = unwind_step(regs)
        if not regs or regs[16] is None or regs[16] < 0x10000: break
    return out


names = {}
if 24 in streams:
    size, rva = streams[24]
    for i in range(struct.unpack_from('<I', d, rva)[0]):
        t, nrva = struct.unpack_from('<IQ', d, rva + 4 + 12 * i)
        try: names[t] = mstr(nrva)
        except Exception: pass

size, rva = streams[6]
ftid = struct.unpack_from('<I', d, rva)[0]
fctx = struct.unpack_from('<I', d, rva + 8 + 32 + 120 + 4)[0]
print(f'faulting thread {ftid} {names.get(ftid, "")}')
for s in walk(ctx_regs(fctx), 60): print('   ', s)

if ALL:
    size, rva = streams[3]
    for k in range(struct.unpack_from('<I', d, rva)[0]):
        o = rva + 4 + 48 * k
        tid = struct.unpack_from('<I', d, o)[0]
        crva = struct.unpack_from('<I', d, o + 44)[0]
        if tid == ftid: continue
        fr = walk(ctx_regs(crva), 40)
        interesting = [f for f in fr if 'eu4.exe' in f or 'eu4boost' in f]
        print(f'thread {tid} {names.get(tid, "")}: ' + ' <- '.join(fr[:6]))
        if interesting: print('      eu4: ' + ' <- '.join(interesting[:12]))
