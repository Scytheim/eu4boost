r"""Where the reverse-engineering data lives. Each path comes from an environment variable, else from
tools/local_paths.json (this PC's paths, not in git), else the default:
  EU4_EXPORT  <folder holding the repo>\ghidra_export   functions.tsv, calls.tsv, string_refs.tsv (ExportInventory.java)
  EU4_DECOMP  <folder holding the repo>\ghidra_decomp   eu4_decomp_000..032.c + index.tsv (DecompileAll.java)
  EU4_EXE     eu4.exe in the default Steam folder      (1.37.5.0, timestamp 0x66FE6AB9)
Example tools/local_paths.json: {"EU4_EXE": "E:\\Games\\Steam\\steamapps\\common\\Europa Universalis IV\\eu4.exe"}
"""
import json, os

_here = os.path.dirname(os.path.abspath(__file__))
_local = {}
_lp = os.path.join(_here, '..', 'local_paths.json')
if os.path.isfile(_lp):
    with open(_lp, encoding='utf-8') as _f:
        _local = json.load(_f)
_work = os.path.abspath(os.path.join(_here, '..', '..', '..'))


def _path(key, default):
    return os.environ.get(key) or _local.get(key) or default


EXPORT = _path('EU4_EXPORT', os.path.join(_work, 'ghidra_export'))
DECOMP = _path('EU4_DECOMP', os.path.join(_work, 'ghidra_decomp'))
EXE = _path('EU4_EXE', r'C:\Program Files (x86)\Steam\steamapps\common\Europa Universalis IV\eu4.exe')
GAME = os.path.dirname(EXE)

FUNCTIONS = os.path.join(EXPORT, 'functions.tsv')     # entry name namespace body_bytes thunk callers callees callconv signature
CALLS = os.path.join(EXPORT, 'calls.tsv')             # caller(function entry) callee
STRING_REFS = os.path.join(EXPORT, 'string_refs.tsv') # function string_address string
IMAGE_BASE = 0x140000000


def rows(path, header_first_col):
    """Yield tab-split rows of a Ghidra export TSV (CRLF or LF), skipping the header."""
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\r\n').split('\t')
            if p[0] != header_first_col:
                yield p
