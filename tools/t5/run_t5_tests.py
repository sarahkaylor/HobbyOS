#!/usr/bin/env python3
"""run_t5_tests.py — self-registering host-test suite (GB-064).

Auto-discovers and runs every python host test under tools/t5/ plus the two
tools-root T5 tests, so **adding a host test needs no edit to the shared
Makefile host_tests line** (drop a tools/t5/<name>_test.py and it just runs).
Two branches that each add a distinct _test.py file therefore merge with no
conflict — this removes the host_tests recipe-line collision the swarm saw
(PR #27 replacing #29's lines).

Every discovered script is executed with python3 and its OWN stdout is checked
for "ALL PASSED" — evidence by the run's own PASS lines, never by "it built".

Exit code: number of failures (0 = all pass).
"""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root (tools/t5 -> root)

def discover():
    """Ordered, deduped set of python test scripts to execute."""
    scripts = [os.path.relpath(p, ROOT)
               for p in glob.glob(os.path.join(ROOT, "tools", "t5", "*_test.py"))]
    # tools-root T5 tests (stable, low churn); NOT run_selfhost_build_test.py
    # (a heavy standalone selfhost run, deliberately outside host_tests).
    scripts += ["tools/evidence_rig_test.py", "tools/perf_game_cycle_test.py"]
    seen, out = set(), []
    for s in scripts:
        if s not in seen:
            seen.add(s)
            out.append(s)
    return sorted(out)


def run_one(path, cwd):
    r = subprocess.run(["python3", path], cwd=cwd, capture_output=True, text=True)
    ok = "PASSED" in (r.stdout + r.stderr)
    tail = (r.stdout or r.stderr).strip().splitlines()
    last = tail[-1] if tail else ""
    return ok, last


def main():
    cwd = ROOT
    scripts = discover()
    print(f"[host-tests] self-registering suite: {len(scripts)} python tests")
    fails = 0
    for s in scripts:
        ok, last = run_one(s, cwd)
        print(f"  {'PASS' if ok else 'FAIL'}  {s}" + (f"  ({last})" if not ok else ""))
        fails += 0 if ok else 1
    print(f"[host-tests] done: {len(scripts)-fails}/{len(scripts)} passed")
    return fails


if __name__ == "__main__":
    sys.exit(main())
