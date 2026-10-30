"""GB-133 root discriminator: for each pte 1->0 transition of the same (pid,page),
compare the AS root of the present sample with the root of the absent sample.

same root  -> the page was unmapped inside one address space (a real unmap)
root changed -> the AS under that pid was replaced (exec/fork); nothing was
                "re-cleared", the commit simply went into a table nobody used.
"""
import re
import sys

probe = re.compile(
    r"\[GB133-R\] (\w+) pid=(\d+) pg=0x([0-9A-F]+) pte=([01])"
    r"(?: root=0x([0-9A-F]+) cr3=0x([0-9A-F]+))?")

log = sys.argv[1]
lines = open(log, errors="replace").read().splitlines()
last_present = {}
same = 0
changed = 0
unknown = 0
examples = []
for i, ln in enumerate(lines):
    m = probe.search(ln)
    if not m:
        continue
    site, pid, pg, pte = m.group(1), m.group(2), m.group(3), m.group(4)
    root = m.group(5)
    key = (pid, pg)
    if pte == "1":
        last_present[key] = (root, i)
    elif pte == "0" and key in last_present:
        old_root, old_i = last_present.pop(key)
        if old_root is None or root is None:
            unknown += 1
        elif old_root == root:
            same += 1
            if len(examples) < 3:
                examples.append(("SAME-ROOT (real unmap)", old_i, i, pid, pg,
                                 root, lines[old_i], lines[i]))
        else:
            changed += 1
            if len(examples) < 6:
                examples.append(("ROOT-CHANGED (AS replaced)", old_i, i, pid, pg,
                                 f"{old_root} -> {root}", lines[old_i], lines[i]))

print(f"probe lines: {sum(1 for ln in lines if '[GB133-R]' in ln)}")
print(f"flips same-root (real unmap): {same}")
print(f"flips root-changed (new AS):  {changed}")
print(f"flips unknown (no root field):{unknown}")
for label, a, b, pid, pg, root, la, lb in examples:
    print(f"\n--- {label} pid={pid} pg=0x{pg} root={root} (lines {a} -> {b})")
    print("    present:", la[:140])
    print("    absent :", lb[:140])