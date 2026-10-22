"""Summarize the game's console timer_dump files (Documents\\Paradox Interactive\\Europa Universalis IV\\logs\\timer_dump_*.txt):
python timer_dump.py file1.txt [file2.txt ...]
Note: these measure only the daily tick (FULL_TICK etc.), not rendering between days."""
import csv, statistics as st, sys

for p in sys.argv[1:]:
    rows = list(csv.reader(open(p)))[1:]
    hdr, data = rows[0], [r for r in rows[1:] if r and r[0][:1].isdigit()]
    ft = [int(r[1]) for r in data]
    second = [int(r[1]) for r in data if r[0].endswith('_02')]
    normal = [int(r[1]) for r in data if not r[0].endswith('_02')]
    cols = {h: sum(int(r[i]) for r in data) for i, h in enumerate(hdr) if i > 0}
    print(f"{p}: {data[0][0]} -> {data[-1][0]}, {len(data)} days, FULL_TICK sum {sum(ft)} mean {st.mean(ft):.1f} median {st.median(ft)} normal {st.mean(normal):.1f} 2nd-of-month {st.mean(second) if second else 0:.1f}")
    print('   ', {k: v for k, v in cols.items() if v})
