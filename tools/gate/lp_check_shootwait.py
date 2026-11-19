#!/usr/bin/env python3
"""lp_check_shootwait.py <serial.log> -- linux_probe_shootwait's console check
(H7-fault round 2, H7-07; docs/cp-fault.md §12).  Over the boot's [PFS] rows,
the mprot entries (n/us/max_us/targets/running/slow): at most 5 % of the
mprotect shootdowns may have had a target that acknowledged 1 ms or more
late, and none may have taken 20 ms or more.  The probe keeps other CPUs
inside the kernel with interrupts off: faults that first-touch guest RAM
(a host page fault, milliseconds under host memory pressure) and faults
spinning for vm_lock behind a long hold.  A 0x82 IPI waits out the whole
stretch, so the targets must not be doing that work themselves.
Exit 0 = pass, 1 = fail, 2 = no run."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] S1 " not in text:
    print("lp_check_shootwait: no '[LP] S1' row (the probe did not run)")
    sys.exit(2)
n = us = mx = slow = 0
for ln in text.splitlines():
    if "[PFS] t=" not in ln:
        continue
    m = re.search(r" mprot=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", ln)
    if m:
        v = [int(x) for x in m.groups()]
        n += v[0]
        us += v[1]
        mx = max(mx, v[2])
        slow += v[5]
pct = 100.0 * slow / n if n else 0.0
ok = n >= 1000 and pct <= 5.0 and mx < 20000
print(f"lp_check_shootwait: {'PASS' if ok else 'FAIL'} -- {n} mprotect shootdowns, "
      f"{slow} slow target acks ({pct:.1f} %), total {us} us, max {mx} us")
sys.exit(0 if ok else 1)
