#!/usr/bin/env python3
"""tools/cp/p0host/read-sizes.py <trace> <until HH:MM:SS.ffffff> — size distribution of every
read/pread64 the traced processes made on game-dir files up to <until> (unfinished/resumed
pairs included).  Prints a power-of-two histogram and the 15 largest single reads."""
import re, sys, collections
G = "/mnt/fast/games/steamapps/common/Cyberpunk 2077/"
trace, until = sys.argv[1], sys.argv[2]
full = re.compile(r"^(\d+) (\S+) (read|pread64)\(\d+<([^>]*)>.*\) = (\d+)$")
unf = re.compile(r"^(\d+) (\S+) (read|pread64)\(\d+<([^>]*)>.*<unfinished \.\.\.>$")
res = re.compile(r"^(\d+) (\S+) <\.\.\. (read|pread64) resumed>.*\) = (\d+)$")
pend = {}; hist = collections.Counter(); bsum = collections.Counter(); big = []; n = tot = 0
def add(r, call, path):
    global n, tot
    n += 1; tot += r; b = 1
    while b < r: b *= 2
    hist[b] += 1; bsum[b] += r; big.append((r, call, path[len(G):]))
for l in open(trace, errors="replace"):
    f = l.split(" ", 2)
    if len(f) > 1 and f[1] > until: break
    if "read" not in l: continue
    l = l.rstrip()
    m = full.match(l)
    if m:
        if m.group(4).startswith(G): add(int(m.group(5)), m.group(3), m.group(4))
        continue
    m = unf.match(l)
    if m:
        if m.group(4).startswith(G): pend[m.group(1)] = (m.group(3), m.group(4))
        continue
    m = res.match(l)
    if m and m.group(1) in pend:
        c, p = pend.pop(m.group(1)); add(int(m.group(4)), c, p)
print(f"# game-dir read/pread64 calls up to {until}: {n}, bytes {tot}")
print("# return-size histogram (bucket = next power of two >= bytes): bucket\tcalls\tbytes")
for b in sorted(hist): print(f"{b}\t{hist[b]}\t{bsum[b]}")
print("# 15 largest single reads: bytes call path")
for r in sorted(big, reverse=True)[:15]: print(*r, sep="\t")
