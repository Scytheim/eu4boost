"""Find instructions that reference an address rip-relatively (globals, vtables, strings):
python dref.py 1423494b4 [more ...]   (needs numpy)"""
import bisect, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, pefile, capstone
from re_paths import EXE, FUNCTIONS, rows

pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
data = text.get_data(); tva = base + text.VirtualAddress
arr = np.frombuffer(data, dtype=np.uint8); n = len(data) - 4
d = (arr[0:n].astype(np.int64) | (arr[1:n + 1].astype(np.int64) << 8) | (arr[2:n + 2].astype(np.int64) << 16) | (arr[3:n + 3].astype(np.int64) << 24))
d = np.where(d >= 2 ** 31, d - 2 ** 32, d)
pos = np.arange(n, dtype=np.int64) + tva
funcs = sorted((int(p[0], 16), p[1]) for p in rows(FUNCTIONS, 'entry')); starts = [f[0] for f in funcs]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for ts in sys.argv[1:]:
    t = int(ts, 16); print('==', ts)
    for extra in (0, 1, 2, 4):                      # bytes of immediate after the displacement
        for i in np.nonzero(pos + 4 + extra + d == t)[0]:
            va = tva + int(i)
            for back in range(2, 9):
                s = va - back
                ins = next(md.disasm(data[s - tva:s - tva + 16], s), None)
                if ins and ins.address + ins.size == va + 4 + extra and 'rip' in ins.op_str:
                    fi = bisect.bisect_right(starts, s) - 1
                    print(f"  {s:x} [{funcs[fi][0]:x} {funcs[fi][1]}] {ins.mnemonic} {ins.op_str}")
                    break
