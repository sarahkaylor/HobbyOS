#!/usr/bin/env python3
"""host_concurrency_gate.py - GB-085 concurrency gate.

Two concurrent `make host_tests` runs used to collide on fixed /tmp paths
(/tmp/TODO.TXT, /tmp/NOTES.TXT, /tmp/ANTFARM.TST, ...) because every host
test wrote to a shared absolute path.  This gate proves the fix:

  NORMAL  : build once, then run TWO `make host_tests` at the same time.
            BOTH must report "Checks failed: 0" (or equivalent) and exit 0.
            With per-process scratch dirs (GB-085) neither run can touch the
            other's files, so both pass.

  NEGATIVE: with HO_TEST_SHARED_DIR set to ONE fixed path, the same pair-run
            must FAIL (at least one run reports failures) -- proving the gate
            can detect a reintroduced shared path and is not passing by luck.

Verdict = exit status plus an explicit PASS/FAIL line (a silent exit 0 is not
a pass).  Run:  python3 tools/host/host_concurrency_gate.py [--negative]
"""
import os, subprocess, sys, time, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PASS_LINES = ("Checks failed: 0", "ALL APPS SUITE TESTS PASSED")


def run_make(logfile):
    """One `make host_tests`, stdout+stderr to logfile. Returns (rc, text)."""
    with open(logfile, "w") as f:
        p = subprocess.run(["make", "host_tests"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    with open(logfile, "r", errors="replace") as f:
        text = f.read()
    return p.returncode, text


def verdict(rc, text, tag):
    passed = any(l in text for l in PASS_LINES)
    if rc == 0 and passed:
        return True
    return False


def main():
    negative = "--negative" in sys.argv
    tmpd = tempfile.mkdtemp(prefix="gb085_gate_")

    # Phase 0: build once (serial), so the pair-run exercises the /tmp race
    # rather than racing make's own object writes.
    print("[gb085] building host_tests once (serial)...", flush=True)
    rc, text = run_make(os.path.join(tmpd, "build.log"))
    if rc != 0 or not any(l in text for l in PASS_LINES):
        print("FAIL: serial build did not pass (rc=%d)" % rc)
        print(text[-2000:])
        return 2
    print("[gb085] serial build green.", flush=True)

    envbase = dict(os.environ)
    if negative:
        # Force one shared scratch path for BOTH runs -> they collide.
        shared = os.path.join(tmpd, "SHARED")
        envbase["HO_TEST_SHARED_DIR"] = shared
        print("[gb085] NEGATIVE mode: HO_TEST_SHARED_DIR=%s" % shared, flush=True)

    # Phase 1: run two `make host_tests` simultaneously.
    logA = os.path.join(tmpd, "runA.log")
    logB = os.path.join(tmpd, "runB.log")
    print("[gb085] starting two make host_tests simultaneously...", flush=True)
    pa = subprocess.Popen(["make", "host_tests"], cwd=ROOT, env=envbase,
                          stdout=open(logA, "w"), stderr=subprocess.STDOUT)
    pb = subprocess.Popen(["make", "host_tests"], cwd=ROOT, env=envbase,
                          stdout=open(logB, "w"), stderr=subprocess.STDOUT)
    ra = pa.wait()
    rb = pb.wait()

    ta = open(logA, errors="replace").read()
    tb = open(logB, errors="replace").read()
    okA = verdict(ra, ta, "A")
    okB = verdict(rb, tb, "B")

    if negative:
        # Must FAIL (at least one run red) to prove the gate can catch a
        # reintroduced shared path.
        if okA and okB:
            print("FAIL (negative): expected a shared path to fail BOTH-safe "
                  "runs, but both passed rc=%d/%d" % (ra, rb))
            return 1
        print("PASS (negative): shared path made the pair-run fail "
              "(A rc=%d ok=%s ; B rc=%d ok=%s)" % (ra, okA, rb, okB))
        print("CONCURRENCY_GATE: PASS (negative control)")
        return 0

    if okA and okB:
        print("PASS: two concurrent make host_tests runs both green "
              "(A rc=%d, B rc=%d)" % (ra, rb))
        print("CONCURRENCY_GATE: PASS")
        return 0

    print("FAIL: concurrent runs not both green (A rc=%d ok=%s ; B rc=%d ok=%s)"
          % (ra, okA, rb, okB))
    for name, text in (("A", ta), ("B", tb)):
        fails = [l for l in text.splitlines() if "FAIL" in l and "NOT" not in l]
        print("---- %s tail ----" % name)
        print("\n".join(text.splitlines()[-15:]))
        if fails:
            print("---- %s FAIL lines ----" % name)
            print("\n".join(fails[-15:]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
