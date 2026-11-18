#!/usr/bin/env python3
"""lp_check_sigint.py <serial.log> -- linux_probe_sigint's console check
(H4-kglitch): its nested handler frames (I6) must not produce a
"[SFX] MISMATCH" row; that row is the kernel's own frame-restore check, and
a nested return is not a mismatch.  Exit 0 = clean, 1 = rows, 2 = no run."""
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] I6 " not in text:
    print("lp_check_sigint: no '[LP] I6' row (the probe did not run)")
    sys.exit(2)
rows = [ln for ln in text.splitlines() if "[SFX] MISMATCH pid=" in ln]
for ln in rows[:4]:
    print("  e.g. " + ln[:200])
print(f"lp_check_sigint: {'FAIL' if rows else 'PASS'} -- {len(rows)} [SFX] MISMATCH row(s)")
sys.exit(1 if rows else 0)
