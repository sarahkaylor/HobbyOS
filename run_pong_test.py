#!/usr/bin/env python3
"""
Run Pong in-OS integration test.
Launches QEMU with the Pong test wrapper, waits for PONG_TEST_PASS
in serial output, and takes a screenshot to validate rendering.
"""
import subprocess
import socket
import json
import time
import sys
import os
import select

QMP_SOCK = "./qmp-pong-sock"
ACTUAL_PPM = "src/user/actual_pong.ppm"

if os.path.exists(QMP_SOCK):
    os.remove(QMP_SOCK)

def log(msg):
    print(msg, flush=True)

def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)

log("Starting QEMU for Pong in-OS test...")
arch = os.environ.get("ARCH", "arm")
process = subprocess.Popen(
    ["make", "pong_test_run", f"ARCH={arch}",
     "QEMU_ARGS=-display none -qmp unix:./qmp-pong-sock,server,nowait"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True
)

ready = False
timeout = 60
start_time = time.time()
line_buf = ""

while True:
    elapsed = time.time() - start_time
    if elapsed > timeout:
        log(f"[PONG_TEST] Timeout waiting for PONG_TEST_PASS (reached {timeout}s)")
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
            if "PONG_TEST_PASS" in line_buf:
                ready = True
                log("\n[PONG_TEST] FOUND PONG_TEST_PASS IN BUFFER!")
                break
            if "PONG_TEST_FAIL" in line_buf:
                log("\n[PONG_TEST] FOUND PONG_TEST_FAIL IN BUFFER!")
                kill_qemu()
                sys.exit(1)
        except Exception as e:
            log(f"[PONG_TEST] Read error: {e}")
            break
    else:
        if process.poll() is not None:
            break

if not ready:
    log("[PONG_TEST] Process exited before PONG_TEST_PASS.")
    kill_qemu()
    sys.exit(1)

# Take a screenshot via QMP
log("[PONG_TEST] Taking screenshot via QMP...")
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(QMP_SOCK)
    greeting = s.recv(4096)
    s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode('utf-8') + b'\n')
    s.recv(4096)
    s.sendall(json.dumps({"execute": "screendump",
                          "arguments": {"filename": ACTUAL_PPM}}).encode('utf-8') + b'\n')
    resp = s.recv(4096).decode('utf-8')
    if "return" not in resp:
        log(f"[PONG_TEST] Unexpected QMP response: {resp}")
        kill_qemu()
        sys.exit(1)
    s.close()
except Exception as e:
    log(f"[PONG_TEST] QMP Error: {e}")
    kill_qemu()
    sys.exit(1)

log("[PONG_TEST] Terminating QEMU...")
kill_qemu()
process.wait()

# Validate screenshot
log("[PONG_TEST] Validating screenshot...")
if not os.path.exists(ACTUAL_PPM):
    log(f"[PONG_TEST] Screenshot '{ACTUAL_PPM}' not found!")
    sys.exit(1)

with open(ACTUAL_PPM, "rb") as f:
    ppm_data = f.read()

pixel_data = ppm_data[100:]
if not any(pixel_data):
    log("[PONG_TEST] Error: Screenshot is completely black!")
    sys.exit(1)

log("[PONG_TEST] Screenshot validation passed (non-blank image confirmed)!")
log("[PONG_TEST] OK")
sys.exit(0)