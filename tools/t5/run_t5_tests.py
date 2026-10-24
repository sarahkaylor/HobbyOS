#!/usr/bin/env python3
"""run_t5_tests.py — self-registering host-test suite (GB-064).

Auto-discovers and runs every python host test under tools/t5/ plus the two
tools-root T5 tests, so **adding a host test needs no edit to the shared
Makefile host_tests line** (drop a tools/t5/<name>_test.py and it just runs).
Two branches that each add a distinct _test.py file therefore merge with no
conflict — this removes the host_tests recipe-line collision the swarm saw
(PR #27 replacing #29's lines).

**The verdict is the test's exit status — never a substring of its stdout.**
Until 2026-10-23 this grader was `ok = "PASSED" in output`: two scripts that
exited 0 and printed their own verdict in a different capitalisation
(`CLAIM_RACE_TEST: PASS — …`, `ACCEPTANCE_GATE_TEST: PASS — …`) were graded
FAIL and reddened the whole tier, while a script that crashed *after* printing
"ALL PASSED" would have been graded PASS. A substring match answers a question
about the wording of the log, not about what the test proved.

A script still has to *state* a verdict, so an exit-0 script that silently
prints nothing cannot count as a pass (the "a skip that prints a pass" class,
GB-007). Both directions are proven by `--self-test`, which must be run with
`--self-test` in CI/host_tests: it fails if the grader cannot grade a known-bad
script as bad.

Exit code: number of failures (0 = all pass).
"""
import glob, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root (tools/t5 -> root)

# A script counts as having stated a verdict if it prints one of these. Both
# spellings of the program's own convention are accepted: "ALL PASSED" /
# "…: PASS" — the point is that the *script* asserted, not the grader.
PASS_MARKERS = ("ALL PASSED", ": PASS", "PASSED")


def discover():
    """Ordered, deduped set of python test scripts to execute."""
    scripts = [os.path.relpath(p, ROOT)
               for p in glob.glob(os.path.join(ROOT, "tools", "*", "*_test.py"))]
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
    r = subprocess.run([sys.executable, path], cwd=cwd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    stated = any(m in out for m in PASS_MARKERS)
    ok = (r.returncode == 0) and stated
    tail = out.strip().splitlines()
    last = tail[-1] if tail else ""
    if r.returncode == 0 and not stated:
        last = "exit 0 but printed no PASS verdict — a silent pass is not a pass"
    return ok, last


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def self_test():
    """Prove the grader can return both verdicts. A grader that cannot fail is not one."""
    d = tempfile.mkdtemp()
    cases = [
        # (name, body, expected_ok, why)
        ("good.py", "print('ALL PASSED')\nraise SystemExit(0)\n", True,
         "exit 0 + stated verdict"),
        ("lowercase.py", "print('SOME_TEST: PASS — fine')\nraise SystemExit(0)\n", True,
         "exit 0 + ': PASS' spelling (the case that reddened the tier)"),
        ("lies.py", "print('ALL PASSED')\nraise SystemExit(1)\n", False,
         "prints PASSED but exits 1 — the substring-only grader called this a pass"),
        ("silent.py", "raise SystemExit(0)\n", False,
         "exit 0 with no verdict statement — a silent skip is not a pass"),
        ("crash.py", "raise SystemExit(3)\n", False, "non-zero exit"),
    ]
    fails = 0
    for name, body, want, why in cases:
        p = os.path.join(d, name)
        write(p, body)
        ok, last = run_one(p, d)
        good = (ok == want)
        print(f"  {'PASS' if good else 'FAIL'}: {name} ({why}) -> grader said {'PASS' if ok else 'FAIL'}, wanted {'PASS' if want else 'FAIL'}")
        fails += 0 if good else 1
    print("RUN_T5_TESTS_SELF_TEST:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
    return fails


def main():
    if "--self-test" in sys.argv:
        return self_test()
    cwd = ROOT
    scripts = discover()
    print(f"[host-tests] self-registering suite: {len(scripts)} python tests")
    grader_fails = self_test()          # the grader proves it can fail, every run
    fails = grader_fails
    total = len(scripts) + 5            # 5 = the grader's own self-test cases
    for s in scripts:
        ok, last = run_one(s, cwd)
        print(f"  {'PASS' if ok else 'FAIL'}  {s}" + (f"  ({last})" if not ok else ""))
        fails += 0 if ok else 1
    print(f"[host-tests] done: {total-fails}/{total} passed")
    return fails


if __name__ == "__main__":
    sys.exit(main())
