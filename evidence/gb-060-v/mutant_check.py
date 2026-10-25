#!/usr/bin/env python3
"""Negative control for GB-060-V: prove locksteal_test_host can FAIL.

Copies src/kernel/arch/x64/locks.c, makes lock_stallbreak_ok() break a LIVE
holder regardless of its heartbeat (the safety property the test claims to
guard), compiles the test against the mutant and runs it.  The tree is not
modified; the mutant lives in the scratch dir and is rebuilt on each run.

Usage: ./mutant_check.sh   (or: python3 mutant_check.py <repo-root>)
"""
import os
import re
import subprocess
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
src = os.path.join(root, "src/kernel/arch/x64/locks.c")
dst = "/tmp/gb060_locks_mutant.c"
binout = "/tmp/gb060_locksteal_mutant"

s = open(src).read()
old = "  return (now_ms - hb) >= LOSTWAKE_DEAD_OWNER_MS;"
assert old in s, "anchor line not found — locks.c changed shape, update this control"
s = s.replace(old, "  (void)hb; return 1; /* MUTANT: break a LIVE holder regardless of heartbeat */", 1)
open(dst, "w").write(s)
print(f"[mutant] wrote {dst} (lock_stallbreak_ok now breaks live holders)")

cc = ["clang", "-Isrc/include", "-Wall", "-Wextra", "-g",
      "-Isrc/user_include", "-Isrc/user_include/graphics", "-DHOST_TEST",
      dst, "src/host/locksteal_test.c", "-o", binout]
r = subprocess.run(cc, cwd=root, capture_output=True, text=True)
print(f"[mutant] compile rc={r.returncode}")
if r.returncode != 0:
    print(r.stderr[-2000:])
    sys.exit(3)

r = subprocess.run([binout], cwd=root, capture_output=True, text=True)
print(r.stdout)
print(f"[mutant] MUTANT_EXIT={r.returncode}  (expected non-zero: the checks must fail)")
sys.exit(0 if r.returncode != 0 else 1)
