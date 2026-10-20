#!/usr/bin/env python3
"""diff_harness_test.py — GB-007 T1 self-test.

Proves the DoD "'hello' diverges / lie detector works" entirely on the host:
builds the hello probe, captures it with diff_harness.py, checks the contract
{stdout, exit, errno} is what hello declares, re-runs (must MATCH), and then
plants three divergent captures (wrong stdout / wrong exit / wrong errno) —
each MUST be reported DIVERGE, never silently passed.

Run: python3 tools/differential/diff_harness_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, "diff_harness.py")
BUILD = os.path.join(HERE, "build_hello.sh")
BIN = os.path.join(HERE, "build", "hello")

g_checks = 0
g_fails = 0


def check(cond, msg):
    global g_checks, g_fails
    g_checks += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        g_fails += 1
        print(f"  FAIL: {msg}")


def harness(args):
    return subprocess.run([sys.executable, HARNESS] + args,
                          capture_output=True, text=True)


def build_hello():
    if shutil.which("cc") is None and shutil.which("gcc") is None:
        return False
    subprocess.run(["bash", BUILD], check=True, capture_output=True)
    return os.path.exists(BIN)


def main():
    print("=== build + capture hello ===")
    if not build_hello():
        print("SKIP: no cc toolchain; cannot build the probe")
        return 0
    check(os.path.exists(BIN), "hello built")

    with tempfile.TemporaryDirectory() as td:
        ref = os.path.join(td, "ref.json")
        again = os.path.join(td, "again.json")
        r1 = harness(["run", "--exe", BIN, "--out", ref, "--runner", "host"])
        r2 = harness(["run", "--exe", BIN, "--out", again, "--runner", "host"])
        check(r1.returncode == 0, "capture ref run")
        check(r2.returncode == 0, "capture re-run")

        rec = json.load(open(ref))
        check(rec["stdout"] == "hello from hobbyos\nerrno=2\n",
              "stdout is the hello contract (hello line + errno line)")
        check(rec["exit"] == 0, "exit is 0")
        check(rec["errno"] == 2, "errno is ENOENT(=2)")

        print("=== identical re-run -> MATCH ===")
        c = harness(["compare", "--ref", ref, "--target", again])
        check(c.returncode == 0 and "MATCH" in c.stdout,
              f"identical captures MATCH ({c.stdout.strip()})")

        print("=== lie detector: planted divergences must be DIVERGE ===")
        base = json.load(open(ref))
        cases = [
            ("wrong stdout", lambda d: d.update({"stdout": "tail -f /dev/null\n"}), "stdout"),
            ("wrong exit", lambda d: d.update({"exit": 1}), "exit"),
            ("wrong errno", lambda d: d.update({"errno": 13}), "errno"),
        ]
        for name, mutate, field in cases:
            bad = json.loads(json.dumps(base))
            mutate(bad)
            pth = os.path.join(td, "bad.json")
            json.dump(bad, open(pth, "w"))
            c = harness(["compare", "--ref", ref, "--target", pth])
            check(c.returncode == 1 and "DIVERGE" in c.stdout and field in c.stdout,
                  f"{name} -> DIVERGE ({c.stdout.strip()})")

    print(f"\nChecks: {g_checks}, failed: {g_fails}")
    if g_fails:
        print("DIFF HARNESS TEST FAILED")
        return 1
    print("DIFF HARNESS TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
