#!/usr/bin/env python3
"""GB-081-V clause 3 — the police control the card demands.

Removes the first `%3o` from the `-b` first-diff printf (cmp_gnu.c:645), rebuilds
the host parity tool, runs it and requires the parity suite to FAIL; then restores
the file from git and requires 55/55 again. Nothing is left modified.

Usage: control.py <repo-root>
"""
import os
import subprocess
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
path = os.path.join(root, "src/user/cmp_gnu.c")

orig = open(path).read()
anchor = 'is %3o %s %3o %s\\n'
assert anchor in orig, "anchor printf not found — cmp_gnu.c changed shape"
mut = orig.replace(anchor, 'is %s %3o %s\\n', 1)   # delete the first %3o
open(path, "w").write(mut)
print("[control] mutant written: first %3o deleted in the -b first-diff printf")


def run(cmd):
    r = subprocess.run(cmd, cwd=root, shell=True, capture_output=True, text=True)
    print(f"$ {cmd}\n{r.stdout.strip()}\n(exit {r.returncode})")
    return r.returncode


run("make cmp_parity_strict")
print("\n[control] expected above: NOT 55/55 (3 FAILED or equivalent)\n")

subprocess.run(["git", "checkout", "--", "src/user/cmp_gnu.c"], cwd=root)
print("[control] file restored from git; diff should be empty:")
print(subprocess.run(["git", "diff", "--stat", "--", "src/user/cmp_gnu.c"],
                     cwd=root, capture_output=True, text=True).stdout or "(empty)")

run("make cmp_parity_strict")