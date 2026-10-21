import re, sys, collections
p = sys.argv[1] if len(sys.argv) > 1 else "obj/t3_intel.log"
raw = open(p, errors="ignore").read()
# tolerate console interleave: match the tag and whatever pid/rdi survived
pat = re.compile(r"\[ENTRY-KBUF\]\s*pid=\s*(?:[^\s=]*?)?(\d+)?[^\n]*?rdi=0x([0-9A-Fa-f]{6,16})")
hits = pat.findall(raw)
print("ENTRY-KBUF occurrences (raw tag count):", raw.count("[ENTRY-KBUF]"))
print("parsed with pid+rdi:", len(hits))
vals = collections.Counter(h[1].lower() for h in hits)
print("distinct rdi values:", len(vals))
for v, c in vals.most_common(8):
    print(f"  rdi=0x{v}  x{c}")
pids = collections.Counter(h[0] for h in hits if h[0])
print("pids seen:", dict(list(pids.items())[:10]))
# how many faults are family-B, and do their pids overlap the ENTRY-KBUF pids?
fb = re.findall(r"\[KERNEL\] pid=(\d+) \([^)]*\) memory fault VA=0x[0-9A-Fa-f]+.*?rw=r", raw)
print("family-B faults:", len(fb), "pids:", sorted(set(fb), key=int)[:12])
print("overlap:", sorted(set(fb) & set(pids), key=int)[:12])
