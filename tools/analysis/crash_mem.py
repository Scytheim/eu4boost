"""python tools/analysis/crash_mem.py minidump.dmp ADDR [LEN]: hex dump of the memory a minidump holds ("..": not in the dump)."""
import bisect, struct, sys
d = open(sys.argv[1], 'rb').read()
addr = int(sys.argv[2], 16); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
ns, dr = struct.unpack_from('<II', d, 8)
ranges = []
for i in range(ns):
    t, size, rva = struct.unpack_from('<III', d, dr + 12 * i)
    if t == 5:
        for k in range(struct.unpack_from('<I', d, rva)[0]):
            s, sz, r = struct.unpack_from('<QII', d, rva + 4 + 16 * k); ranges.append((s, sz, r))
    if t == 9:
        nr, off = struct.unpack_from('<QQ', d, rva)
        for k in range(nr):
            s, sz = struct.unpack_from('<QQ', d, rva + 16 + 16 * k); ranges.append((s, sz, off)); off += sz
ranges.sort(); st = [r[0] for r in ranges]
def rd(a):
    i = bisect.bisect_right(st, a) - 1
    if i < 0: return None
    s, sz, off = ranges[i]
    return d[off + a - s] if a < s + sz else None
near = [r for r in ranges if r[0] <= addr + n and addr - 0x10000 <= r[0] + r[1]]
print('ranges near:', [(hex(r[0]), hex(r[1])) for r in near[:8]])
for row in range(addr, addr + n, 16):
    b = [rd(row + k) for k in range(16)]
    if all(x is None for x in b): continue
    print(f'{row:#014x}: ' + ' '.join('..' if x is None else f'{x:02x}' for x in b))
