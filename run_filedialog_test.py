#!/usr/bin/env python3
"""
Run the file dialog arrow key test inside QEMU.

This script boots HobbyOS in QEMU with MODE=filedialog_test, which loads
FILEDIAL.BIN (the file dialog arrow key test). The test:
  1. Launches the editor
  2. Opens the File > Open dialog
  3. Sends DOWN arrow key events
  4. Checks if the > selection marker moved down
  5. Sends UP arrow key events
  6. Checks if the > selection marker moved up

The test prints FILEDIALOG_ARROW_TEST: PASS or FAIL to serial console.
This script captures that output and reports the result.
"""
import subprocess
import time
import sys
import os
import select

with open("debug_filedialog.log", "w") as f:
    f.write("File dialog arrow key test started\n")

def log(msg):
    print(msg, flush=True)
    with open("debug_filedialog.log", "a") as f:
        f.write(msg + "\n")

def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)

arch = os.environ.get("ARCH", "arm")
log(f"Starting QEMU for file dialog test (ARCH={arch})...")

process = subprocess.Popen(
    # NOTE: QEMU_CMD in the Makefile already includes "-serial stdio"; do NOT
    # add it again here or QEMU aborts with "cannot use stdio by multiple
    # character devices". We only need to force the display off for headless
    # runs (matches run_pong_test.py / run_desktop_test.py).
    ["make", "filedialog_test_run", f"ARCH={arch}",
     "QEMU_ARGS=-display none"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True
)

timeout = 60
start_time = time.time()
result = None
line_buf = ""

while True:
    elapsed = time.time() - start_time
    if elapsed > timeout:
        log(f"[TEST] Timeout waiting for test result (reached {timeout}s)")
        kill_qemu()
        sys.exit(1)

    r, _, _ = select.select([process.stdout], [], [], 1.0)
    if process.stdout in r:
        try:
            data = os.read(process.stdout.fileno(), 4096)
            if not data:
                break
            text = data.decode('utf-8', errors='ignore')
            sys.stdout.write(text)
            sys.stdout.flush()
            line_buf += text

            if "FILEDIALOG_ARROW_TEST: PASS" in line_buf:
                result = "PASS"
                log("[TEST] Got PASS result!")
                break
            elif "FILEDIALOG_ARROW_TEST: FAIL" in line_buf:
                result = "FAIL"
                log("[TEST] Got FAIL result!")
                break
        except Exception as e:
            log(f"[TEST] Read error: {e}")
            break
    else:
        if process.poll() is not None:
            break

kill_qemu()
process.wait()

if result == "PASS":
    log("[TEST] RESULT: PASS - Arrow keys work in file dialog")
    sys.exit(0)
elif result == "FAIL":
    log("[TEST] RESULT: FAIL - Arrow keys do NOT work in file dialog")
    sys.exit(1)
else:
    log("[TEST] RESULT: UNKNOWN - No result received from QEMU")
    # Print the last 2000 chars of output for debugging
    if line_buf:
        log("[TEST] Last output: " + line_buf[-2000:])
    sys.exit(1)