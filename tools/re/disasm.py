"""Disassemble eu4.exe: python disasm.py 0x140179e90 [bytes=400]
Annotates IAT calls, string references and known function names.
(Was dis.py; renamed because a file called dis.py shadows Python's stdlib module and breaks capstone.)"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile, capstone
from re_paths import EXE, FUNCTIONS, STRING_REFS, rows

pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
iat = {i.address: (i.name or b'?').decode() for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}
names = {int(p[0], 16): p[1] for p in rows(FUNCTIONS, 'entry')}
strs = {int(p[1], 16): p[2] for p in rows(STRING_REFS, 'function')}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def dis(va, size):
    off = va - base
    for ins in md.disasm(img[off:off + size], va):
        s = f"{ins.address:x}: {ins.mnemonic} {ins.op_str}"
        m = re.search(r'rip ([+-]) 0x([0-9a-f]+)', ins.op_str)
        if m:
            t = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
            if t in iat: s += f"   ; IAT {iat[t]}"
            elif t in strs: s += f"   ; \"{strs[t][:80]}\""
            else: s += f"   ; [{t:x}]"
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            t = int(ins.op_str, 16)
            if t in names: s += f"   ; {names[t]}"
        print(s)


if __name__ == '__main__':
    dis(int(sys.argv[1], 16), int(sys.argv[2], 0) if len(sys.argv) > 2 else 400)
