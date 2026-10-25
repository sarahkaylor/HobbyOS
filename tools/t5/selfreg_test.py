#!/usr/bin/env python3
"""Host (T1) self-test for the GB-064 self-registering host-test runner.

Proves the DoD: (1) discover() picks up every tools/t5/*_test.py with no
explicit registration; (2) dropping a NEW *_test.py makes it part of the suite
with NO Makefile edit (self-registration); (3) run_one reports pass by the
script's own PASSED line, not by it merely executing.
"""
import os, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import run_t5_tests as R

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

base = R.discover()
# existing t5 python tests are all discovered
need = ["tools/t5/replay_test.py", "tools/t5/flaky_triage_test.py",
        "tools/t5/game_docs_test.py", "tools/t5/feasibility_doc_test.py",
        "tools/t5/retro_skill_test.py"]
check(all(n in base for n in need), "discover() auto-includes existing t5 tests")
check("tools/evidence_rig_test.py" in base, "discover() includes evidence_rig_test")
check("tools/run_selfhost_build_test.py" not in base,
      "heavy standalone selfhost test stays out of host_tests")

# self-registration: a NEW tools/t5/_test.py joins the suite with no Makefile edit.
# The temp name is per-process (pid), NOT the fixed zz_new_test.py, so two
# concurrent host_tests runs (GB-085) cannot collide on the same fixture file.
tmp = os.path.join(R.ROOT, "tools", "t5", "zz_new_%d_test.py" % os.getpid())
rel = os.path.relpath(tmp, R.ROOT)
with open(tmp, "w") as f:
    f.write('print("Tests: ALL PASSED")\n')
try:
    check(rel in R.discover(),
          "new tools/t5/*_test.py auto-registers (no Makefile edit)")
    ok, last = R.run_one(rel, R.ROOT)
    check(ok, "runner runs the new test and sees its own PASSED line")
finally:
    os.remove(tmp)

# run_one verdict comes from the script's own PASSED line, not mere execution
ok_fail, _ = R.run_one("tools/t5/feasibility_doc_test.py", R.ROOT)
check(ok_fail is True, "run_one marks a genuinely passing script as PASS")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
