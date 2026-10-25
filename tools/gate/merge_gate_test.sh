#!/usr/bin/env bash
# tools/gate/merge_gate_test.sh — the merge-gate DoD self-test (GB-005).
#
# Proves the DoD: "a dummy branch fails -> passes the gate."
#
#   Part A (verdict logic, fast, no QEMU): drive merge_gate.sh police — the
#   T3 verdict over a captured console log.  A clean log must be GREEN; a log
#   that is byte-identical except for one injected FAIL line (a "dummy branch"
#   outputting a failure) must be RED.  This is the fails->passes property,
#   proven by construction on the exact detector the gate inherits.
#
#   Part B (real gate, the honest tier): run the full merge_gate.sh with its
#   T1 tier on this working tree and require GREEN (make host_tests, which also
#   carries t4 parity).  Heavy QEMU tiers stay off (SKIP, never silent pass).
#
# Usage:  bash tools/gate/merge_gate_test.sh   (fast: Part A)
#         TEST_RUN_T1=1 bash tools/gate/merge_gate_test.sh  (Part A + real T1 gate)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p obj/gate/test

fails=0
pass()  { echo "  PASS: $1"; }
fail()  { fails=1; echo "  FAIL: $1"; }

# ---- Part A: T3 verdict logic (fails->passes) ---------------------------
echo "Part A: T3 gate verdict over a captured log (no QEMU)"
CLEAN="obj/gate/test/t3_clean.log"; DUMMY="obj/gate/test/t3_dummy.log"
{
  for i in $(seq 1 1200); do echo "console line $i"; done
  echo "System halt"
} > "$CLEAN"
# dummy = byte-identical except one FAIL line (a defective branch's output)
{ for i in $(seq 1 600); do echo "console line $i"; done; echo "SHELLTEST WATCHDOG: protocol stalled — FAIL"; for i in $(seq 601 1200); do echo "console line $i"; done; echo "System halt"; } > "$DUMMY"

bash tools/gate/merge_gate.sh police intel "$CLEAN" >/dev/null 2>&1
[ $? -eq 0 ] && pass "clean log -> GREEN" || fail "clean log should be GREEN"

bash tools/gate/merge_gate.sh police intel "$DUMMY" >/dev/null 2>&1
[ $? -ne 0 ] && pass "dummy (FAIL-injected) log -> RED" || fail "dummy log should be RED"

# and the reverse for a FATAL kernel-mode fault (the 'console about a wreck'
# case the coordinator called out: a crashed kernel also prints System halt)
DUMMYK="obj/gate/test/t3_fatal.log"
{ for i in $(seq 1 1200); do echo "console line $i"; done; echo "FATAL kernel-mode fault at vector=6"; echo "System halt"; } > "$DUMMYK"
bash tools/gate/merge_gate.sh police intel "$DUMMYK" >/dev/null 2>&1
[ $? -ne 0 ] && pass "dummy (FATAL fault) log -> RED" || fail "FATAL-fault log should be RED"

# ---- Part C: the verdict names what ran (GB-005, 2026-10-25) -------------
# The Coordinator refused a run that printed `GATE: PASS` while T2/T3 read
# `skip` (PR #43, 2026-10-18).  Two properties are asserted here, both fast and
# both real runs of the shipped script:
#   C1 every tier disabled -> exit 0, and the verdict NAMES the skipped tiers;
#   C2 a merge-gate run (GATE_CI=1) that drops a core tier -> exit 1, refusing
#      before it does any work, rather than passing over the absence.
echo "Part C: verdict completeness (what ran is named; a merge gate may not drop a tier)"
GATE_T1=0 GATE_T2=0 GATE_T3=0 GATE_STYLE=0 bash tools/gate/merge_gate.sh \
    >obj/gate/test/gate_allskip.log 2>&1
if [ $? -eq 0 ] && grep -q "GATE: PASS (ran: none; skipped: .*T1_host.*T3_int_intel" \
    obj/gate/test/gate_allskip.log; then
  pass "all-skip run names every skipped tier"
else
  fail "all-skip run must PASS and name the skipped tiers"
  tail -6 obj/gate/test/gate_allskip.log | sed 's/^/    /'
fi
if grep -q "GATE: PASS (no required tier failed" obj/gate/test/gate_allskip.log; then
  fail "the old bare 'GATE: PASS (no required tier failed…)' line is back"
else
  pass "no bare PASS line on a run with skips"
fi
GATE_CI=1 GATE_T1=0 bash tools/gate/merge_gate.sh >obj/gate/test/gate_drop.log 2>&1
if [ $? -ne 0 ] && grep -q "may not drop a core tier: T1_host" obj/gate/test/gate_drop.log; then
  pass "GATE_CI=1 with a dropped core tier -> RED (names the tier)"
else
  fail "GATE_CI=1 with T1 dropped must refuse, naming the tier"
  tail -6 obj/gate/test/gate_drop.log | sed 's/^/    /'
fi

# ---- Part B (optional): real T1 gate on this tree -----------------------
if [ "${TEST_RUN_T1:-0}" = "1" ]; then
  echo "Part B: real gate, T1 tier (make host_tests)"
  if bash tools/gate/merge_gate.sh >obj/gate/test/gate_t1.log 2>&1; then
    pass "merge_gate.sh (T1) -> GREEN"
    grep -E "^\[GATE\] T1_host: PASS|GATE: PASS" obj/gate/test/gate_t1.log | sed 's/^/    /'
  else
    fail "merge_gate.sh (T1) should be GREEN on a clean tree"
    tail -40 obj/gate/test/gate_t1.log | sed 's/^/    /'
  fi
else
  echo "Part B: skipped (TEST_RUN_T1=1 to run the real T1 gate)"
fi

echo
if [ "$fails" = "1" ]; then echo "MERGE_GATE_TEST: FAIL"; exit 1; fi
echo "MERGE_GATE_TEST: PASS — a dummy branch fails, a clean branch passes"
exit 0
