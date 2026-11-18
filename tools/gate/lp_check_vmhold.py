#!/usr/bin/env python3
"""lp_check_vmhold.py <serial.log> -- linux_probe_vmhold's console check
(H7-fault round 2, H7-10): no fault of the run may have waited 1 ms or
more for vm_lock ([PFX] F rows, lw= in us) while the main thread mprotects,
MAP_FIXED-replaces and munmaps a resident 256 MiB region: those calls walked
every page under vm_lock in one hold.  Exit 0 = pass, 1 = fail, 2 = no run."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] H1 " not in text:
    print("lp_check_vmhold: no '[LP] H1' row (the probe did not run)")
    sys.exit(2)
rows = [ln[ln.index("[PFX] F"):] for ln in text.splitlines() if "[PFX] F" in ln]
waits = []
for r in rows:
    m = re.search(r" lw=(\d+)", r)
    if m and int(m.group(1)) >= 1000:
        waits.append(r)
for r in waits[:4]:
    print("  waited: " + r[:200])
mx = max([int(m.group(1)) for m in (re.search(r" lw=(\d+)", r) for r in rows) if m] or [0])
print(f"lp_check_vmhold: {'PASS' if not waits else 'FAIL'} -- {len(rows)} long fault row(s), "
      f"{len(waits)} with lw >= 1000 us (max lw {mx} us)")
sys.exit(0 if not waits else 1)
