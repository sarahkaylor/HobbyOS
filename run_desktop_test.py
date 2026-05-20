#!/usr/bin/env python3
import subprocess
import socket
import json
import time
import sys
import os
import select

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

def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)

log("Starting QEMU for desktop test...")
arch = os.environ.get("ARCH", "arm")
process = subprocess.Popen(
    ["make", "desktop_test_run", f"ARCH={arch}", "QEMU_ARGS=-display none -qmp unix:./qmp-sock,server,nowait"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True
)

ready = False
timeout = 30
start_time = time.time()
line_buf = ""

while True:
    elapsed = time.time() - start_time
    if elapsed > timeout:
        log(f"[TEST] Timeout waiting for SCREENSHOT_READY (reached {timeout}s)")
        kill_qemu()
        sys.exit(1)
        
    # Wait for up to 1 second for data to be ready to read
    r, _, _ = select.select([process.stdout], [], [], 1.0)
    if process.stdout in r:
        try:
            # Use os.read to bypass Python's TextIOWrapper buffering
            data = os.read(process.stdout.fileno(), 1024)
            if not data:
                break
            text = data.decode('utf-8', errors='ignore')
            sys.stdout.write(text)
            sys.stdout.flush()
            
            line_buf += text
            if "SCREENSHOT_READY" in line_buf:
                ready = True
                log("\n[TEST] FOUND READY IN BUFFER!")
                break
        except Exception as e:
            log(f"[TEST] Read error: {e}")
            break
    else:
        # Check if the process exited prematurely
        if process.poll() is not None:
            break

if not ready:
    log("[TEST] Process exited before ready.")
    kill_qemu()
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
        kill_qemu()
        sys.exit(1)
        
    s.close()
except Exception as e:
    log(f"[TEST] QMP Error: {e}")
    kill_qemu()
    sys.exit(1)

log("[TEST] Terminating QEMU...")
kill_qemu()
process.wait()

log("[TEST] Validating screenshot...")
if not os.path.exists(EXPECTED_PPM):
    log(f"[TEST] Expected screenshot '{EXPECTED_PPM}' not found!")
    sys.exit(1)

log("[TEST] OK")
sys.exit(0)
