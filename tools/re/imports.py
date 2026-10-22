"""List a PE's imports: python imports.py [path=eu4.exe] [dll-filter]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile
from re_paths import EXE

path = sys.argv[1] if len(sys.argv) > 1 else EXE
pe = pefile.PE(path, fast_load=False)
for e in pe.DIRECTORY_ENTRY_IMPORT:
    dll = e.dll.decode()
    names = [(hex(i.address), (i.name.decode() if i.name else f"ord{i.ordinal}")) for i in e.imports]
    print(f"## {dll} ({len(names)})")
    if len(sys.argv) > 2 and sys.argv[2].lower() in dll.lower():
        for a, n in names: print("  ", a, n)
