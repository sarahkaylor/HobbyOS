#!/usr/bin/env python3
"""Cwd-scoped replacement for run_apps_test.py (shared-host safe).

Same flow as run_apps_test.py (make MODE=apps_test run -> watch the serial
-> verdict), but it NEVER runs a broad pkill: it records pre-existing qemu
PIDs first, and at the end kills only qemu processes whose /proc/<pid>/cwd
is the target tree (i.e. the QEMU it just started), plus its own make
wrapper.  Safe to run while other lanes/agents boot their own QEMUs.

Known pre-existing issue (2026-10-14, not theme-related): the harness
stops at TASKS.BIN ("interact - window vanished during the functional
check") on both the Chrome-light tree and the untouched pre-theme probe
tree @ 722e3b4; see theme.md section 5.

Usage:
  python3 tools/run_apps_test_scoped.py [workdir] [tag] [timeout_s]
    workdir    tree to build/boot in          (default: repo root)
    tag        log/file tag                   (default: scoped)
    timeout_s  verdict wait budget in seconds (default: 420)
Serial log: /tmp/apps_test_scoped_<tag>.log
"""
import glob
import os
import subprocess
import sys
import time


def qemu_pids():
    pids = []
    for comm in glob.glob("/proc/[0-9]*/comm"):
        try:
            if "qemu-system" in open(comm, errors="ignore").read():
                pids.append(comm.split("/")[2])
        except OSError:
            pass
    return pids


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    workdir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else repo
    tag = sys.argv[2] if len(sys.argv) > 2 else "scoped"
    timeout_s = int(sys.argv[3]) if len(sys.argv) > 3 else 420
    os.chdir(workdir)
    serial = f"/tmp/apps_test_scoped_{tag}.log"
    pre = set(qemu_pids())
    print(f"[scoped] workdir={workdir} pre-existing qemus={sorted(pre)}")

    logf = open(serial, "w")
    proc = subprocess.Popen(
        ["make", "apps_test_run", "QEMU_ARGS=-display none"],
        stdout=logf, stderr=subprocess.STDOUT)

    verdict = None
    start = time.time()
    while time.time() - start < timeout_s:
        try:
            text = open(serial, errors="ignore").read()
        except OSError:
            text = ""
        if "APPS TEST PASSED" in text:
            verdict = "pass"
            break
        if "APPS TEST FAILED" in text:
            verdict = "fail"
            break
        if proc.poll() is not None:
            time.sleep(1)
        time.sleep(0.5)

    # Kill ONLY the qemus this run started: not pre-existing, cwd == workdir.
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

    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    logf.close()

    text = open(serial, errors="ignore").read()
    n_pass = text.count("[APPS_T] PASS ")
    print(f"[scoped] VERDICT={verdict} n_pass={n_pass}/10 log={serial}")
    print(text[-1500:])
    return 0 if verdict == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
