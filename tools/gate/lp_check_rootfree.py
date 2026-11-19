#!/usr/bin/env python3
"""lp_check_rootfree.py <serial.log> -- linux_probe_rootfree's console check
(H8-root, docs/cp-root.md): no address space's page tables may be freed while
another CPU may still translate through them.

The kernel counts every vm_arch_teardown in its [ROOTFREE] rows:
  onroot=  teardowns that found another CPU on the root (its bookkeeping or
           its real CR3);
  unsafe=  ... whose frames were freed anyway (base: every onroot);
and prints a [FREERACE] row for each CPU found on a root as its frames are
freed.  PASS needs [ROOTFREE] rows (the running totals) that never count an unsafe
free, and no [FREERACE] row.
Exit 0 = pass, 1 = fail, 2 = no run / no instrument."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] R1 " not in text:
    print("lp_check_rootfree: no '[LP] R1' row (the probe did not run)")
    sys.exit(2)
rows = [ln[ln.index("[ROOTFREE]"):] for ln in text.splitlines() if "[ROOTFREE]" in ln]
races = [ln[ln.index("[FREERACE]"):] for ln in text.splitlines() if "[FREERACE]" in ln]
if not rows:
    print("lp_check_rootfree: no [ROOTFREE] row (kernel without the H8-root record?)")
    sys.exit(2)
# the rows are running totals (every 64th teardown, every race, the halt):
# grade the largest, and any row that ever counted an unsafe free
def fields(r):
    return {k: int(v) for k, v in re.findall(r" (\w+)=(\d+)", r)}
last = max(rows, key=lambda r: fields(r).get("td", 0))
for r in races[:6]:
    print("  " + r[:200])
unsafe = max(fields(r).get("unsafe", 0) for r in rows)
ok = unsafe == 0 and not races
print(f"lp_check_rootfree: {'PASS' if ok else 'FAIL'} -- {last.strip()[:240]}; "
      f"{len(races)} [FREERACE] row(s)")
sys.exit(0 if ok else 1)
