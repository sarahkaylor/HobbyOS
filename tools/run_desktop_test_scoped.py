#!/usr/bin/env python3
"""Cwd-scoped replacement for run_desktop_test.py (shared-host safe).

Same acceptance flow as run_desktop_test.py (make desktop_test_run ->
wait for SCREENSHOT_READY -> QMP screendump -> non-blank validation),
but at the end it kills ONLY the qemu it started (cwd == workdir, not in
the pre-existing set) instead of the broad `pkill qemu-system-*` that
run_desktop_test.py performs.  Mirrors the stock runner's boot-time
bound (RUNNER_READY_TIMEOUT) but is overridable via argv for hosts where
the ARM TCG boot is slower than the stock 30 s (a host-speed artifact,
not an acceptance difference).

Usage:
  python3 tools/run_desktop_test_scoped.py [workdir] [tag] [ready_timeout_s]
    workdir           tree to build/boot in          (default: repo root)
    tag               log/file tag                   (default: scoped)
    ready_timeout_s   SCREENSHOT_READY wait budget    (default: 30, stock)
"""
import glob
import json
import os
import select
import socket
import subprocess
import sys
import time

QMP_SOCK = "/tmp/dt-qmp-sock"  # instance-scoped below
ACTUAL_PPM = "/tmp/dt-actual.ppm"


def qemu_pids():
    pids = []
    for comm in glob.glob("/proc/[0-9]*/comm"):
        try:
            if "qemu-system" in open(comm, errors="ignore").read():
                pids.append(comm.split("/")[2])
        except OSError:
            pass
    return pids


def kill_own_qemu(workdir, pre):
    for pid in qemu_pids():
        if pid in pre:
            continue
        try:
            cwd = os.readlink(f"/proc/{pid}/cwd")
        except OSError:
            continue
        if cwd == workdir:
            os.kill(int(pid), 9)
            print(f"[scoped] killed own qemu pid={pid}")


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    workdir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else repo
    tag = sys.argv[2] if len(sys.argv) > 2 else "scoped"
    ready_timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    os.chdir(workdir)
    log = f"/tmp/desktop_test_scoped_{tag}.log"
    qmp = f"/tmp/dt-qmp-{tag}.sock"
    ppm = f"/tmp/dt-actual-{tag}.ppm"
    if os.path.exists(qmp):
        os.remove(qmp)
    pre = set(qemu_pids())
    print(f"[scoped] workdir={workdir} pre-existing qemus={sorted(pre)}")

    logf = open(log, "w")
    proc = subprocess.Popen(
        ["make", "desktop_test_run", "ARCH=arm",
         f"QEMU_ARGS=-display none -qmp unix:{qmp},server,nowait"],
        stdout=logf, stderr=subprocess.STDOUT)

    ready = False
    start = time.time()
    while time.time() - start < ready_timeout:
        try:
            text = open(log, errors="ignore").read()
        except OSError:
            text = ""
        if "SCREENSHOT_READY" in text:
            ready = True
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    if not ready:
        print(f"[scoped] TIMEOUT waiting for SCREENSHOT_READY "
              f"({ready_timeout}s) - mirroring stock runner verdict")
        kill_own_qemu(workdir, pre)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        logf.close()
        print("[scoped] VERDICT=fail (ready timeout)")
        sys.exit(1)

    print("[scoped] SCREENSHOT_READY seen; screendump via QMP")
    ok = False
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(qmp)
        greeting = s.recv(4096)
        s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode() + b"\n")
        s.recv(4096)
        s.sendall(json.dumps({"execute": "screendump",
                              "arguments": {"filename": ppm}}).encode() + b"\n")
        resp = s.recv(4096).decode("utf-8", errors="ignore")
        ok = "return" in resp
        s.close()
    except Exception as e:
        print(f"[scoped] QMP Error: {e}")
        ok = False

    kill_own_qemu(workdir, pre)
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    logf.close()

    if not ok or not os.path.exists(ppm):
        print("[scoped] VERDICT=fail (screendump failed)")
        sys.exit(1)

    with open(ppm, "rb") as f:
        ppm_data = f.read()
    pixel_data = ppm_data[100:]
    non_blank = any(pixel_data)
    verdict = "pass" if non_blank else "fail"
    print(f"[scoped] screendump {'NON-BLANK' if non_blank else 'BLANK'} "
          f"{len(ppm_data)} B -> VERDICT={verdict}")
    return 0 if verdict == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
