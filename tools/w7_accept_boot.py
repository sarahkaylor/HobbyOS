#!/usr/bin/env python3
"""w7_accept_boot.py — lane w7-accept: prove an OUT.IMG boots and runs an
in-OS-built program, straight from the image (mission A/B S4/S5).

Boots the given image with the exact acceptance QEMU line (acceptance.md
§5.1), waits for the kernel marker, then — when a program is requested —
launches it from the desktop Apps menu via QMP (run_console_test.py
precedent, the same path run_selfhost_build_test.py's S5 uses) and asserts
the loader's [LAUNCH] line + the [KERNEL] Process … exited line on serial.

Why not run_selfhost_build_test.py directly: that runner stages a boot-disk
with SELFHOST.BIN and runs the S0-S3 in-OS build; here the artifact under
test IS the standalone OUT.IMG, so we boot it directly (S4/S5 semantics).

Usage:
  python3 tools/w7_accept_boot.py --image OUT.IMG --serial out.log
      [--launch OUTPROG.BIN] [--launch-wait 90] [--boot-wait 150]
Exit 0 when every requested assertion holds, 1 on a failed assertion,
2 on boot/timeout failure, 3 on harness error.
"""
import argparse
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import run_selfhost_build_test as H  # reuse boot/Q/stop/menu machinery


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--launch", default=None, help="program to launch from Apps menu")
    ap.add_argument("--launch-wait", type=int, default=90)
    ap.add_argument("--boot-wait", type=int, default=150)
    ap.add_argument("--menu-wait", type=int, default=45)
    ap.add_argument("--no-launch", action="store_true")
    args = ap.parse_args()

    img = os.path.abspath(args.image)
    serial = os.path.abspath(args.serial)
    if not os.path.exists(img):
        print("[w7boot] image not found: %s" % img)
        return 3

    qmp = serial + ".qmp"
    q = H.boot(img, serial, qmp=qmp)
    res = {"image": img, "serial": serial}
    try:
        t0 = time.time()
        el = H.wait_for(serial, H.BOOT_MARKER, args.boot_wait)
        res["boot_marker"] = "ok %.1fs" % el if el else "NOT SEEN"
        print("[w7boot] kernel marker '%s': %s" % (H.BOOT_MARKER, res["boot_marker"]))
        if el is None:
            res["verdict"] = "boot_failure"
            print("[w7boot] VERDICT: boot_failure")
            return 2

        # record the boot-mode line (which mode the OUT.IMG kernel picks)
        txt = H.read_serial(serial)
        m = re.search(r"Mode: [A-Z]+[^\n]*", txt)
        if m:
            res["mode_line"] = m.group(0)
            print("[w7boot] %s" % m.group(0))

        if args.no_launch or not args.launch:
            res["verdict"] = "boot_only_ok"
            print("[w7boot] VERDICT: boot_only_ok (kernel boots from OUT.IMG)")
            return 0

        if "[MENU]" not in H.read_serial(serial):
            H.wait_for(serial, "[MENU]", args.menu_wait)
        txt = H.read_serial(serial)
        idx = H.menu_index(txt, args.launch)
        res["menu_index"] = idx
        if idx is None:
            res["verdict"] = "program_not_in_menu"
            print("[w7boot] VERDICT: %s not in Apps menu" % args.launch)
            return 1

        qc = H.Q(qmp)
        H.launch_driver(qc, idx)
        res["launch_clicked"] = True

        probe = "[LAUNCH] %s" % args.launch
        deadline = time.time() + args.launch_wait
        res["launch_line"] = False
        res["exit_line"] = False
        while time.time() < deadline:
            t = H.read_serial(serial)
            if probe in t:
                res["launch_line"] = True
            if "Process " in t and args.launch in t and "exited" in t:
                res["exit_line"] = True
            if res["launch_line"]:
                break
            time.sleep(0.5)
        print("[w7boot] launch_line=%s exit_line=%s" % (res["launch_line"], res["exit_line"]))
        if not res["launch_line"]:
            res["verdict"] = "launch_not_seen"
            print("[w7boot] VERDICT: launch_not_seen (%s within %ds)" % (probe, args.launch_wait))
            return 1
        res["verdict"] = "pass"
        print("[w7boot] VERDICT: pass — %s launched from OUT.IMG Apps menu" % args.launch)
        return 0
    finally:
        H.stop_qemu(q)


if __name__ == "__main__":
    sys.exit(main())
