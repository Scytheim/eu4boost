"""Compare daily game-state checksum CSVs (eu4boost_state_*.csv, [debug] state_checksum=1) with a baseline:
python compare_state.py ../../validation/baseline_seed1234_20261015.csv run1.csv [run2.csv ...]
Columns c0..c139 are the game's own multiplayer OOS checksums (c0 = RNG call count, c1 = RNG seed).
Only meaningful with [debug] fixed_seed set (same value as the baseline: 1234)."""
import csv, sys

base = list(csv.reader(open(sys.argv[1])))
for path in sys.argv[2:]:
    X = list(csv.reader(open(path)))
    n = min(len(base), len(X))
    bad = [i for i in range(1, n) if X[i] != base[i]]
    if not bad:
        print(f"{path}: {len(X) - 1} days, {n - 1} compared -> IDENTICAL to baseline"); continue
    i = bad[0]
    cols = [j - 1 for j in range(1, len(X[i])) if X[i][j] != base[i][j]]
    print(f"{path}: {len(bad)} of {n - 1} days differ; first {X[i][0]}: categories {cols[:30]}")
    if X[i][2] != base[i][2]: print("   c1 (RNG seed) differs -> fixed_seed not active or different seed")
