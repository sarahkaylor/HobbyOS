#!/usr/bin/env python3
"""
Run the in-OS desktop application harness (MODE=apps_test -> APPS_T.BIN).

The harness boots the desktop with mock input and drives every desktop
application through a full launch/verify/close cycle, printing
"APPS TEST PASSED" or "APPS TEST FAILED (...)" on the serial console.
"""
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

SERIAL_LOG = "/tmp/apps_test_serial.log"


def log(msg):
    print(msg, flush=True)


def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)


def main():
    kill_qemu()
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    log("[INFO] booting APPS_TEST harness...")
    logf = open(SERIAL_LOG, "w")
    proc = subprocess.Popen(
        ["make", "apps_test_run", "ARCH=arm", "QEMU_ARGS=-display none"],
        stdout=logf, stderr=subprocess.STDOUT)

    timeout_s = 120
    start = time.time()
    verdict = None
    while time.time() - start < timeout_s:
        if proc.poll() is not None and verdict is None:
            # Kernel self-halted; give the log a moment to flush.
            time.sleep(1)
        try:
            text = open(SERIAL_LOG, errors="ignore").read()
        except OSError:
            text = ""
        if "APPS TEST PASSED" in text:
            verdict = "pass"
            break
        if "APPS TEST FAILED" in text:
            verdict = "fail"
            break
        time.sleep(0.5)

    kill_qemu()
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    if verdict == "pass":
        log("[TEST] APPS TEST PASSED")
        return 0
    if verdict == "fail":
        log("[TEST] APPS TEST FAILED - last serial output:")
        tail = open(SERIAL_LOG, errors="ignore").read()[-3000:]
        log(tail)
        return 1
    log(f"[TEST] Timeout after {timeout_s}s waiting for APPS TEST verdict "
        "- treating as deadlock/failure (see /tmp/apps_test_serial.log)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
