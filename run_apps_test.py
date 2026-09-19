#!/usr/bin/env python3
"""
Run the in-OS desktop application harness (MODE=apps_test -> APPS_T.BIN).

The harness boots the desktop with mock input and drives every desktop
application through a full launch/verify/close cycle, printing
"APPS TEST PASSED" or "APPS TEST FAILED (...)" on the serial console.

The harness itself exits 0 on success / 1 with a reason on failure; when its
process exits the kernel halts QEMU (no processes left). This driver also
kills QEMU explicitly after the verdict (or on timeout) so a stuck run can
never leave a hanging qemu behind.
"""
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

# ARCH=arm (default) or ARCH=intel, overridable from the environment like
# run_desktop_test.py does.
ARCH = os.environ.get("ARCH", "arm")
SERIAL_LOG = f"/tmp/apps_test_serial_{ARCH}.log"
TIMEOUT_S = 300


def log(msg):
    print(msg, flush=True)


def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)


def read_log():
    try:
        return open(SERIAL_LOG, errors="ignore").read()
    except OSError:
        return ""


def main():
    kill_qemu()
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    log(f"[INFO] booting APPS_TEST harness (ARCH={ARCH})...")
    logf = open(SERIAL_LOG, "w")
    proc = subprocess.Popen(
        ["make", "apps_test_run", f"ARCH={ARCH}", "QEMU_ARGS=-display none"],
        stdout=logf, stderr=subprocess.STDOUT)

    start = time.time()
    verdict = None
    while time.time() - start < TIMEOUT_S:
        if proc.poll() is not None and verdict is None:
            # Kernel self-halted (or make failed); give the log a moment to flush.
            time.sleep(1)
        text = read_log()
        if "APPS TEST PASSED" in text:
            verdict = "pass"
            break
        if "APPS TEST FAILED" in text:
            verdict = "fail"
            break
        time.sleep(0.5)

    # Always terminate qemu + the make wrapper, then flush the log.
    kill_qemu()
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    logf.close()

    text = read_log()
    n_pass = text.count("[APPS_T] PASS ")
    tail = text[-4000:]

    if verdict == "pass":
        log(f"[TEST] APPS TEST PASSED ({n_pass}/10 per-app PASS lines)")
        log(tail)
        return 0
    if verdict == "fail":
        log("[TEST] APPS TEST FAILED - last serial output:")
        log(tail)
        return 1
    log(f"[TEST] Timeout after {TIMEOUT_S}s waiting for APPS TEST verdict "
        f"- treating as deadlock/failure (see {SERIAL_LOG})")
    log(tail)
    return 1


if __name__ == "__main__":
    sys.exit(main())
