#!/usr/bin/env python3
"""lp_check_census.py <serial.log> -- linux_probe_census's console check
(H7-fault round 3, H7-08; docs/cp-fault.md §22).  The probe keeps 8 forked
children (3 threads each) alive for 13 s.  PASS when the census printed
their [THRS] rows (at least 8 rows in some 5 s sample) and every [PFR] row's
t32_census maximum is under 2000 us: the timer interrupt only asks for the
census, a kernel task prints it.  Exit 0 = pass, 1 = fail, 2 = no run."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] C1 " not in text:
    print("lp_check_census: no '[LP] C1' row (the probe did not run)")
    sys.exit(2)
thrs = [ln for ln in text.splitlines() if ln.startswith("[THRS] ") and "PROBE" in ln]
mx = 0
n = 0
for m in re.finditer(r"\[PFR\] t=\d+ .*?t32_census=(\d+)/(\d+)/(\d+)", text):
    n += 1
    mx = max(mx, int(m.group(3)))
ok = len(thrs) >= 8 and n >= 1 and mx < 2000
print(f"lp_check_census: {'PASS' if ok else 'FAIL'} -- {len(thrs)} probe [THRS] rows, "
      f"{n} [PFR] rows, t32_census max {mx} us")
sys.exit(0 if ok else 1)
