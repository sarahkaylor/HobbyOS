import re
import sys

path = sys.argv[1]
pat = re.compile(r"\[GB122-COMMIT\] site=(\w+) br=(\w) pid=(\d+) parent=(-?\d+) sp=0x([0-9A-F]+) page=0x([0-9A-F]+)")
counts = {}
order = []
for line in open(path, errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    key = (m.group(1), m.group(2), m.group(3), m.group(6))
    counts[key] = counts.get(key, 0) + 1
    order.append(key)

print("total commits:", len(order))
print("distinct (site,br,pid,page):", len(counts))
print("repeat offenders (same page committed more than once):")
for k, v in sorted(counts.items(), key=lambda kv: -kv[1])[:8]:
    if v > 1:
        print("  ", k, "x", v)
bysite = {}
bybr = {}
for (s, b, p, pg), v in counts.items():
    bysite[s] = bysite.get(s, 0) + v
    bybr[b] = bybr.get(b, 0) + v
print("by site:", bysite, " by branch:", bybr)