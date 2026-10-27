#!/usr/bin/env python3
"""GB-116-V2 clause 6: run the LAUNCH the way tools/t5/acceptance_run.py does it
(`launch_from_menu(q, slot)`, no gesture) against a real intel guest, and count
[LAUNCH] in the serial.

If this is 0 while the CLI's default (`--gesture keys`) is 1, then the shared helper's
default is a no-op and the acceptance instrument no longer opens the menu.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import evidence_rig as R  # noqa: E402

os.chdir(ROOT)
arch = sys.argv[1] if len(sys.argv) > 1 else "intel"
out = os.path.join(ROOT, "evidence/gb-116-v2", f"acceptance_shape_{arch}.serial.log")
qmp = f"/tmp/gb116v2-shape-{arch}.sock"
for p in (out, qmp):
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass

qemu_make = ["make", "run", f"ARCH={arch}", f"QEMU_ARGS=-display none -qmp unix:{qmp},server,nowait"]
q = R.boot(qemu_make, out, qmp, arch)
try:
    menu = open(out, errors="ignore").read()
    slot = R.find_menu_slot(out, "XNIBBLER.BIN")
    print(f"slot for XNIBBLER.BIN: {slot}")
    R.launch_from_menu(q, slot)          # <-- exactly acceptance_run.py:200
    time.sleep(6)
    serial = open(out, errors="ignore").read()
    print(f"[LAUNCH] count (no gesture, acceptance_run shape): {serial.count('[LAUNCH]')}")
    for line in serial.splitlines():
        if "[LAUNCH]" in line:
            print("  ", line.strip())
finally:
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"])