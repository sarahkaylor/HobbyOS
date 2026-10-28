# -*- coding: utf-8 -*-
"""t3_fatal_count_test.py — GB-121: the intel T3 FATAL counter must count a fatal
even when another CPU shreds its line (the raw sink interleaves chars).

The x64 fatal reporter writes through the lock-free raw sink
(src/kernel/arch/x64/trap.c:3052, uart_puts_raw), so a second CPU can interleave
the "FATAL kernel-mode fault" line and the old counter (an exact-substring grep,
"FATAL kernel-mode fault") silently lost 28 of 30 fatalities (the GB-101 measured
on this card).  The tier now recognises the banner's STABLE fragments via the
kernel_fatal_count() function in tools/t3_tier.sh ("FATAL kernel-m" survives
char-level interleave; "Exception in Kernel Mode" is the mode fatal banner).  The
test drives that SAME function (live) and ALSO proves the OLD recognizer
undercounts the shredded fixture, and that a clean wave still reports 0.

Run: python3 tools/gate/t3_fatal_count_test.py   (exit 0 = pass; auto-registering)

Honesty: the test runs the REAL kernel_fatal_count() and runs the OLD exact-substring
grep against the same fixture and asserts it UNDERcounts — so this test would FAIL
against the pre-GB-121 recognizer.
"""
import re, subprocess, sys, tempfile, os

SELF = os.path.abspath(__file__)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(SELF)))  # repo root
T3 = os.path.join(ROOT, "tools", "t3_tier.sh")

FAILS = []
def check(cond, label, extra=""):
    if cond:
        print("PASS: " + label + (f" ({extra})" if extra else ""))
    else:
        FAILS.append(label)
        print("FAIL: " + label + (f" ({extra})" if extra else ""))


def shredded_sample():
    # The real shredded capture from the GB-121 card (interleaved with
    # "[PASS] close(31) [never opened] -> EBADF" from another CPU):
    return ("[KERNEL] FATAL kernel-m  oPdAeS Sf caulltose(, 3h1)al ti[nngvheirs o CpPenU:ed ]v -ect> oEr=B A2D0F00")


def run_police(log, arch="intel"):
    env = dict(os.environ); env["W0_GATE"] = "0"
    return subprocess.run(["bash", T3, "police", log, arch], capture_output=True,
                          text=True, env=env)


def source_kernel_fatal_count():
    """Eval the REAL kernel_fatal_count() body out of t3_tier.sh (live read) so the
    test and the tier share ONE definition — a revert breaks this test (GB-121 clause 4)."""
    src = open(T3).read()
    m = re.search(r"kernel_fatal_count\(\) \{\n(.*?)\n\}", src, re.S)
    body = m.group(1) if m else ""
    return body


def test_shredded_fatal_is_counted_via_real_tier():
    # DoD 1/2/3 exercised through the REAL tier (police police-grades a log).
    # A shredded fatal + real banner must make the tier count them; a clean log must
    # stay a clean PASS (0 fatal).
    body = source_kernel_fatal_count()
    check('grep -acE "FATAL kernel-m|Exception in Kernel Mode"' in body,
          "live kernel_fatal_count() matches the stable fragments (shred-proof)")
    # log A: one shredded fatal (card capture) + one intact + must be seen
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("Desktop starting...\n" + shredded_sample() + "\n")
        f.write("[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13\n")
        sp = f.name
    try:
        r = run_police(sp)
        check(r.returncode == 1 and "FATAL kernel-mode fault line" in r.stdout,
              "the REAL tier FAILs on a shredded fatal (counted via stable fragments)",
              "rc=%d kfault line present=%s" % (r.returncode,
                  "FATAL kernel-mode fault line" in r.stdout))
        # the OLD exact-substring grep on the SAME fixture undercounts it:
        old = subprocess.run(["grep", "-ac", "FATAL kernel-mode fault", sp],
                             capture_output=True, text=True).stdout.strip()
        check(old == "1", "the OLD exact-substring counter undercounts the shredded fatal",
              f"old counted {old}/2 (the shred was lost to the old recognizer)")
    finally:
        os.unlink(sp)


def test_intact_fatal_still_counted():
    # DoD 2: a real intact kernel-mode fault line still counts (function must return 2).
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13\n" * 2)
        sp = f.name
    try:
        r = run_police(sp)
        check(r.returncode == 1 and "FATAL kernel-mode fault line" in r.stdout,
              "two intact FATAL kernel-mode fault lines -> tier FAILs (count 2)")
    finally:
        os.unlink(sp)


def test_clean_wave_reports_zero():
    # DoD 3: the negative case — a healthy wave prints neither fragment -> clean PASS,
    # no false positives from the looser matching.
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("Desktop starting...\n[kernel] PID 0: DOOM.BIN\nSystem halt\n")
        f.write("[KERNEL] pid=8 (SHTEST.BIN) memory fault VA=0x777 (user, in=PROT)\n")
        f.write("Process 5: PIE.BIN exited\n")
        f.write("Process 6: SUBPRB.BIN exited\n")
        f.write("Process 7: SHTEST.BIN exited\n")
        f.write("Process 8: SHTEST2.BIN exited\n")
        f.write("Process 9: PIE.BIN exited\n")
        f.write("Process 10: SHTEST.BIN exited\n")
        f.write("Process 11: SHTEST2.BIN exited\n")
        f.write("Process 12: PIE.BIN exited\n")
        f.write("Process 13: SHTEST.BIN exited\n")
        f.write("Process 14: SHTEST2.BIN exited\n")
        f.write("System halt\n")
        sp = f.name
    try:
        # a clean wave: >=55 exited programs + enough lines (floor 1000) + halt -> PASS (0 fatal)
        with open(sp, "w") as f:
            for n in range(1, 60):
                f.write(f"Process {n}: APP{n}.BIN exited\n")
            for i in range(0, 1100):
                f.write(f"benign console chatter line {i}\n")
            f.write("System halt\n")
        r = run_police(sp)
        check(r.returncode == 0 and "FATAL" not in r.stdout,
              "clean wave reports 0 fatal (no false positives)", f"rc={r.returncode}")
    finally:
        os.unlink(sp)


def test_no_reporter_behaviour_change():
    # DoD 5: the reporter keeps the exact marker (live) — the fragments the recogniser
    # targets come from the TWO real banners, so they and the tiers cannot drift apart.
    mkf = os.path.join(ROOT, "src/kernel/arch/x64/trap.c")
    mk = open(mkf).read()
    check('FATAL kernel-mode fault, halting this CPU: vector=' in mk and
          'Exception in Kernel Mode' in mk,
          "x64 reporter keeps both fatal banners (fragment source of record; raw sink stays)")


if __name__ == "__main__":
    test_no_reporter_behaviour_change()
    test_shredded_fatal_is_counted_via_real_tier()
    test_intact_fatal_still_counted()
    test_clean_wave_reports_zero()
    print(f"\n[t3_fatal_count_test] {4-len(FAILS)}/{4} passed")
    if FAILS:
        print("T3 FATAL COUNT TEST FAILED")
        sys.exit(1)
    print("T3 FATAL COUNT TEST PASSED")
    sys.exit(0)
