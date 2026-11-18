#!/usr/bin/env python3
"""lp_check_away.py <serial.log> -- linux_probe_away's console check
(H4-kglitch round 2, H5-05): the kernel's [AWAY] instrument must name both
of the probe's absences -- a nanosleep (B=35 or 230) and a user busy loop (U>=4)
-- with ms between 70 and 200.  Exit 0 = both, 1 = missing, 2 = no run."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] W1 " not in text:
    print("lp_check_away: no '[LP] W1' row (the probe did not run)")
    sys.exit(2)
rows = [ln for ln in text.splitlines() if "[AWAY] tid=" in ln]
sleep = busy = 0
for ln in rows:
    m = re.search(r" ms=(\d+)", ln)
    ms = int(m.group(1)) if m else 0
    if not 70 <= ms <= 200:
        continue
    if re.search(r" B=(\d+:\d+,)*(35|230):\d+", ln):
        sleep += 1
    u = re.search(r" U=(\d+)", ln)
    if u and int(u.group(1)) >= 4:
        busy += 1
for ln in rows[:4]:
    print("  e.g. " + ln[:220])
ok = sleep > 0 and busy > 0
print(f"lp_check_away: {'PASS' if ok else 'FAIL'} -- {len(rows)} [AWAY] row(s): "
      f"{sleep} nanosleep absence(s), {busy} busy absence(s)")
sys.exit(0 if ok else 1)
