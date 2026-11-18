#!/usr/bin/env python3
"""lp_check_badelr.py <serial.log> -- linux_probe_badelr's console check
(H4-kglitch): the scheduler must not print [BADELR] for a thread resumed in
a legal low mapping (the probe spins at 0x140000000, where the game's PE
image lives).  Exit 0 = no [BADELR] row, 1 = rows found, 2 = no probe run."""
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] B1 " not in text:
    print("lp_check_badelr: no '[LP] B1' row (the probe did not run)")
    sys.exit(2)
rows = [ln for ln in text.splitlines() if "[BADELR]" in ln]
for ln in rows[:4]:
    print("  e.g. " + ln[:200])
print(f"lp_check_badelr: {'FAIL' if rows else 'PASS'} -- {len(rows)} [BADELR] row(s)")
sys.exit(1 if rows else 0)
