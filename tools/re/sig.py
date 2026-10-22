"""Print the first N bytes at RVAs/VAs of eu4.exe as a C initializer (for the signature checks in src/):
python sig.py 0x179e90 0x1a13b48 [--n 16]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile
from re_paths import EXE, IMAGE_BASE

n = 16
args = [a for a in sys.argv[1:]]
if '--n' in args: i = args.index('--n'); n = int(args[i + 1]); del args[i:i + 2]
pe = pefile.PE(EXE, fast_load=True)
print(f"TimeDateStamp 0x{pe.FILE_HEADER.TimeDateStamp:08X}  SizeOfImage 0x{pe.OPTIONAL_HEADER.SizeOfImage:X}")
for a in args:
    v = int(a, 16); rva = v - IMAGE_BASE if v >= IMAGE_BASE else v
    print(f"RVA 0x{rva:x}: {{ " + ','.join(f'0x{b:02x}' for b in pe.get_data(rva, n)) + " }")
