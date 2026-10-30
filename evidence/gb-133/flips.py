"""GB-133: find pte 1->0 transitions per pid in a [GB133-R] stream and show what
surrounds them in the serial log."""
import re
import sys

log = sys.argv[1]
probe = re.compile(r"\[GB133-R\] (\w+) pid=(\d+) pg=0x([0-9A-F]+) pte=([01])")
ops = re.compile(r"\[GB133\] op=(\w+) seq=(\d+) base=0x([0-9A-F]+)")

lines = open(log, errors="replace").read().splitlines()
state = {}
flips = []
op_idx = []
for i, ln in enumerate(lines):
    m = ops.search(ln)
    if m:
        op_idx.append((i, m.group(1), m.group(3)))
        continue
    m = probe.search(ln)
    if not m:
        continue
    pid, pg, pte = m.group(2), m.group(3), m.group(4)
    key = (pid, pg)
    prev = state.get(key)
    if prev == "1" and pte == "0":
        flips.append((i, pid, pg))
    state[key] = pte

print(f"probe lines: {sum(1 for ln in lines if '[GB133-R]' in ln)}")
print(f"op markers: {len(op_idx)}")
print(f"1->0 flips: {len(flips)}")
for i, pid, pg in flips[:6]:
    print(f"\n--- flip at line {i} pid={pid} pg=0x{pg} ---")
    for j in range(max(0, i - 4), min(len(lines), i + 2)):
        print("   ", lines[j][:150])