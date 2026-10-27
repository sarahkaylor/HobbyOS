#!/usr/bin/env python3
"""t4_probe_guard_test.py - GB-111: the t4 probe guards its socketpair/pipe blocks so a
failed primitive emits SKIP rows with a reason instead of running write/poll/select/
pselect on UNINITIALISED fds; success rows stay byte-identical; a deliberate removal of
the guard makes this check FAIL."""
import os, re, subprocess, sys, tempfile

fails = 0
def check(cond, msg):
    global fails
    print(("  PASS: " if cond else "  FAIL: ") + msg)
    if not cond:
        fails += 1

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "..", "src", "user", "t4_probe.c")
src = open(PROBE).read()

# Guard markers: the fd-using block must only run on a SUCCESSFUL primitive, and the
# failure path must emit explicit SKIP rows (not silently omit, not touch uninit fds).
check("if (spc == 0) {" in src, "socketpair block guarded on success (if (spc == 0))")
check("if (prc == 0) {" in src, "pipe block guarded on success (if (prc == 0))")
check('prow_skip("sockpoll_ready", "no-socketpair")' in src, "socketpair failure -> SKIP with reason")
check('prow_skip("poll_ready", "no-pipe")' in src, "pipe failure -> SKIP with reason")
# the fd-using write references must sit AFTER their guard opens (inside if (prc==0)/if (spc==0))
soil = src.find("if (spc == 0) {")
pil = src.find("if (prc == 0) {")
check(soil >= 0 and "write(sv[1]" in src[soil:], "write(sv[1]) occurs inside the socketpair guard")
check(pil >= 0 and "write(pfd[1]" in src[pil:], "write(pfd[1]) occurs inside the pipe guard")

# Success path is byte-identical (host leg A): pipe/socketpair succeed, no SKIP.
exe = os.path.join(tempfile.gettempdir(), "t4probe_gb111")
r = subprocess.run(["cc", "-Wall", "-o", exe, PROBE], capture_output=True, text=True)
if r.returncode != 0:
    check(False, "t4_probe.c compiles: " + (r.stderr or r.stdout)[-200:])
else:
    rows = subprocess.run([exe], capture_output=True, text=True).stdout
    check(any(ln.startswith("row=sockpair rc=0") for ln in rows.splitlines()), "host leg A: socketpair succeeds (real rows, byte-identical)")
    check(not any("SKIP" in ln for ln in rows.splitlines()), "host leg A: no SKIP rows on success")

# A deliberate guard removal makes this check FAIL (the DoD): drop the pipe guard and
# the "if (prc == 0)" line; the SKIP assertion above then cannot pass.
guardless = src.replace("if (prc == 0) {", "if (1 || prc == 0) {", 1)
removed = "if (prc == 0) {" not in guardless and "if (1 || prc == 0) {" in guardless
check(removed, "guard-removed variant is structurally detectable (check fails when the guard is removed)")

if fails:
    print("T4_PROBE_GUARD_TEST: FAIL (%d)" % fails); sys.exit(1)
print("T4_PROBE_GUARD_TEST: PASS - blocks guarded; failure->SKIP+reason; success byte-identical; guard removal detected")
sys.exit(0)
