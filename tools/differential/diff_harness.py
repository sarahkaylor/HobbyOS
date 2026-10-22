#!/usr/bin/env python3
"""diff_harness.py — GB-007 differential harness skeleton.

Captures the observable contract of a binary -- {stdout, exit code, errno} --
as a JSON capture file, and compares a reference capture against a target
capture, emitting MATCH or DIVERGE.

The contract is deliberately small so it applies invariantly to a HobbyOS-intel
run (QEMU + a runner script that surfaces the binary's stdout/exit/errno-line)
the same way it applies to the host reference run.

Capture record (JSON):
    {"stdout": "...", "exit": 0, "errno": 2, "runner": "host|target"}

Commands:
    diff_harness.py run --exe PATH --out CAP.json [--runner name]
        Run PATH, capturing stdout + exit + the trailing "errno=N" line that
        the hello probe prints, into CAP.json.
    diff_harness.py compare --ref REF.json --target TARGET.json
        Compare two captures.  Prints "MATCH" and exits 0 when all three
        fields agree, "DIVERGE" and exits 1 on any difference.

The "lie detector" (DoD: "hello diverges / lie detector works") is the compare
path: a doctored capture -- wrong stdout, wrong exit, or wrong errno -- MUST
be reported as DIVERGE, never silently passed.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ERRNO_RE = re.compile(rb"errno=(-?\d+)\s*$")


def run_capture(exe, out, runner):
    """Run exe, capture {stdout, exit, errno}, write to out."""
    proc = subprocess.run([exe], capture_output=True, timeout=30)
    stdout = proc.stdout.decode("utf-8", "replace")
    # errno from the hello probe's trailing "errno=N" line.
    errno_val = None
    m = ERRNO_RE.search(proc.stdout)
    if m:
        errno_val = int(m.group(1))
    rec = {"stdout": stdout, "exit": proc.returncode, "errno": errno_val,
           "runner": runner}
    with open(out, "w") as f:
        json.dump(rec, f, indent=2)
    return rec


def compare_captures(ref, target):
    """Return (verdict, [diffs])."""
    diffs = []
    if ref.get("stdout") != target.get("stdout"):
        diffs.append("stdout")
    if ref.get("exit") != target.get("exit"):
        diffs.append("exit")
    if ref.get("errno") != target.get("errno"):
        diffs.append("errno")
    if diffs:
        return "DIVERGE", diffs
    return "MATCH", []


def main(argv):
    ap = argparse.ArgumentParser(prog="diff_harness.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run a binary and capture its contract")
    r.add_argument("--exe", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--runner", default="host")
    c = sub.add_parser("compare", help="compare ref vs target captures")
    c.add_argument("--ref", required=True)
    c.add_argument("--target", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "run":
        run_capture(a.exe, a.out, a.runner)
        print(f"captured {a.exe} -> {a.out}")
        return 0

    with open(a.ref) as f:
        ref = json.load(f)
    with open(a.target) as f:
        target = json.load(f)
    verdict, diffs = compare_captures(ref, target)
    print(f"{verdict}" + (f" ({', '.join(diffs)})" if diffs else ""))
    return 0 if verdict == "MATCH" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
