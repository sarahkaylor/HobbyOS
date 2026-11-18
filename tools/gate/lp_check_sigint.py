#!/usr/bin/env python3
"""lp_check_sigint.py <serial.log> -- linux_probe_sigint's console check
(H4-kglitch): its nested handler frames (I6) must not produce a
"[SFX] MISMATCH" row; that row is the kernel's own frame-restore check, and
a nested return is not a mismatch.  The SIGINT send/deliver rows (H2-F2's
instrument) must be there.  Exit 0 = clean, 1 = fail, 2 = no run."""
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] I6 " not in text:
    print("lp_check_sigint: no '[LP] I6' row (the probe did not run)")
    sys.exit(2)
rows = [ln for ln in text.splitlines() if "[SFX] MISMATCH pid=" in ln]
for ln in rows[:4]:
    print("  e.g. " + ln[:200])
# H2-F2's instrument: the kernel names every SIGINT it queues and delivers
sends = sum(1 for ln in text.splitlines() if "[SIGINT-S] from=" in ln)
dels = sum(1 for ln in text.splitlines() if "[SIGINT-D] pid=" in ln)
bad = bool(rows) or not sends or not dels
print(f"lp_check_sigint: {'FAIL' if bad else 'PASS'} -- {len(rows)} [SFX] MISMATCH row(s), "
      f"{sends} [SIGINT-S] and {dels} [SIGINT-D] row(s) (both must be > 0)")
sys.exit(1 if bad else 0)
