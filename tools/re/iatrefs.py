"""List eu4.exe call/jmp/mov references to imports of a DLL: python iatrefs.py pops_api [-v]"""
import bisect, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile
from re_paths import EXE, FUNCTIONS, rows

pe = pefile.PE(EXE, fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
data = text.get_data(); tva = base + text.VirtualAddress
iat = {i.address: (e.dll.decode(), i.name.decode() if i.name else f"ord{i.ordinal}") for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}
funcs = sorted((int(p[0], 16), p[1], int(p[3])) for p in rows(FUNCTIONS, 'entry'))
starts = [f[0] for f in funcs]
owner = lambda va: funcs[bisect.bisect_right(starts, va) - 1]
want = sys.argv[1].lower()
res = {}
for m in re.finditer(rb'\xff[\x15\x25]', data):              # call/jmp [rip+d]
    off = m.start(); tgt = tva + off + 6 + struct.unpack_from('<i', data, off + 2)[0]
    if tgt in iat and want in iat[tgt][0].lower(): res.setdefault(iat[tgt][1], []).append(tva + off)
for m in re.finditer(rb'[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', data):   # mov r64,[rip+d]
    off = m.start(); tgt = tva + off + 7 + struct.unpack_from('<i', data, off + 3)[0]
    if tgt in iat and want in iat[tgt][0].lower(): res.setdefault(iat[tgt][1], []).append(tva + off)
for name, sites in sorted(res.items(), key=lambda x: -len(x[1])):
    print(f"{len(sites):5d} {name}")
    if len(sys.argv) > 2:
        for s in sites: f = owner(s); print(f"        {s:x} in {f[0]:x} {f[1]} size={f[2]}")
