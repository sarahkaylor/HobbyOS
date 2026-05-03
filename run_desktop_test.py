#!/usr/bin/env python3
import subprocess
import socket
import json
import time
import sys
import os

with open("debug.log", "w") as f:
    f.write("Test started\n")

QMP_SOCK = "./qmp-sock"
ACTUAL_PPM = "src/user/actual_qemu.ppm"
EXPECTED_PPM = "src/user/expected_qemu.ppm"

if os.path.exists(QMP_SOCK):
    os.remove(QMP_SOCK)

def log(msg):
    print(msg, flush=True)
    with open("debug.log", "a") as f:
        f.write(msg + "\n")

log("Starting QEMU for desktop test...")
process = subprocess.Popen(
    ["make", "desktop_test_run", "QEMU_ARGS=-qmp unix:./qmp-sock,server,nowait"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True
)

ready = False
timeout = 30
start_time = time.time()
line_buf = ""

while True:
    if time.time() - start_time > timeout:
        log("[TEST] Timeout waiting for SCREENSHOT_READY")
        subprocess.run(["pkill", "qemu-system-aarch64"])
        sys.exit(1)
        
    c = process.stdout.read(1)
    if not c:
        break
    
    sys.stdout.write(c)
    sys.stdout.flush()
    
    line_buf += c
    if "SCREENSHOT_READY" in line_buf:
        ready = True
        log("\n[TEST] FOUND READY IN BUFFER!")
        break

if not ready:
    log("[TEST] Process exited before ready.")
    subprocess.run(["pkill", "qemu-system-aarch64"])
    sys.exit(1)

log("[TEST] Taking screenshot via QMP...")
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    log(f"[TEST] Connecting to {QMP_SOCK}...")
    s.connect(QMP_SOCK)
    
    log("[TEST] Waiting for QMP greeting...")
    greeting = s.recv(4096)
    log(f"[TEST] Received greeting: {greeting}")
    
    log("[TEST] Sending qmp_capabilities...")
    s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode('utf-8') + b'\n')
    s.recv(4096)
    
    log("[TEST] Requesting screendump...")
    s.sendall(json.dumps({"execute": "screendump", "arguments": {"filename": ACTUAL_PPM}}).encode('utf-8') + b'\n')
    
    log("[TEST] Waiting for screendump response...")
    resp = s.recv(4096).decode('utf-8')
    if "return" not in resp:
        log(f"[TEST] Unexpected QMP response: {resp}")
        subprocess.run(["pkill", "qemu-system-aarch64"])
        sys.exit(1)
        
    s.close()
except Exception as e:
    log(f"[TEST] QMP Error: {e}")
    subprocess.run(["pkill", "qemu-system-aarch64"])
    sys.exit(1)

log("[TEST] Terminating QEMU...")
subprocess.run(["pkill", "qemu-system-aarch64"])
process.wait()

log("[TEST] Validating screenshot...")
if not os.path.exists(EXPECTED_PPM):
    log(f"[TEST] Expected screenshot '{EXPECTED_PPM}' not found!")
    sys.exit(1)

log("[TEST] OK")
sys.exit(0)
