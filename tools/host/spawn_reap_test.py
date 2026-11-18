#!/usr/bin/env python3
"""spawn_reap_test.py -- a host test must not leave the programs it spawned.

editor_test_host drives the real desktop code, which launches EDITOR.BIN
through spawn2(); the host mock (src/host/compat.c) forks and execs
./EDITOR.BIN_host for it.  The test then exit()s from its flush callback and
nothing ever reaped the child: every `make host_tests` left one
./EDITOR.BIN_host asleep in a pipe read, re-parented to init, forever
(strix, 2026-11-17: about 70 of them across the lane trees, up to 2 days old).

This runs editor_test_host in its own process group and fails if any member
of that group is still alive after the test has exited.  The group is killed
either way, so a red run does not add to the leak.  Self-registers through
tools/t5/run_t5_tests.py (tools/*/*_test.py).
"""
import os
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST = "editor_test_host"


def group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def main():
    os.chdir(ROOT)
    b = subprocess.run(["make", "-s", TEST, "EDITOR.BIN_host"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if b.returncode != 0 or not os.access("./" + TEST, os.X_OK):
        print(b.stdout[-2000:])
        print("spawn_reap_test: FAIL -- could not build %s" % TEST)
        return 1
    # Output to a file, not a pipe: a leaked child inherits the pipe and would
    # keep it open, so "read to EOF" would hang on exactly the defect.
    logf = tempfile.TemporaryFile()
    p = subprocess.Popen(["./" + TEST], stdout=logf, stderr=subprocess.STDOUT,
                         start_new_session=True)
    try:
        p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        print("spawn_reap_test: FAIL -- %s did not finish in 60 s" % TEST)
        return 1
    logf.seek(0)
    out = logf.read().decode("utf-8", "replace")
    if p.returncode != 0 or "PASS" not in out:
        if group_alive(p.pid):
            os.killpg(p.pid, signal.SIGKILL)
        print(out[-2000:])
        print("spawn_reap_test: FAIL -- %s itself failed (rc=%d)" % (TEST, p.returncode))
        return 1
    # The children are killed by the test's own exit path; give them a moment.
    deadline = time.time() + 3.0
    while group_alive(p.pid) and time.time() < deadline:
        time.sleep(0.1)
    if group_alive(p.pid):
        rows = subprocess.run(["ps", "-A", "-o", "pid=,pgid=,ppid=,command="],
                              stdout=subprocess.PIPE, text=True).stdout.splitlines()
        left = "\n".join(r for r in rows
                         if len(r.split()) > 1 and r.split()[1] == str(p.pid))
        os.killpg(p.pid, signal.SIGKILL)
        print("still running after %s exited:\n%s" % (TEST, left))
        print("spawn_reap_test: FAIL -- %s left spawned processes behind" % TEST)
        return 1
    print("spawn_reap_test: PASS -- %s passed and left no spawned process" % TEST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
