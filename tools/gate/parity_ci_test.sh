#!/usr/bin/env bash
# tools/gate/parity_ci_test.sh — the GB-044 parity-CI self-test.
#
# Proves the two-arch parity + run-window-budget verdict:
#   * two green arches               -> PARITY PASS
#   * one red arch  (arm or intel)   -> PARITY FAIL
#   * window over the budget         -> PARITY FAIL  (the GB-044 DoD)
#   * concurrent window <= sequential window (the mechanism that keeps the
#     run under the best-known baseline)
#   * real wiring: the t3 verdict over a real log, per arch (no QEMU)
#
# Uses stub per-arch commands (PARITY_ARM_CMD/PARITY_INTEL_CMD) so it is fast
# and deterministic; one case exercises a real tools/t3_tier.sh police log.
# Usage:  bash tools/gate/parity_ci_test.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p obj/parity/test

fails=0
pass() { echo "  PASS: $1"; }
fail() { fails=1; echo "  FAIL: $1"; }

run_section() { echo "--- $1 ---"; }

# 1) both green -> PASS
run_section "1. both arches green -> PARITY PASS"
PARITY_SEQUENTIAL=1 PARITY_ARM_CMD=true PARITY_INTEL_CMD=true \
  bash tools/gate/parity_ci.sh >obj/parity/test/t1.log 2>&1
[ $? -eq 0 ] && pass "two green arches -> PASS" || { fail "two green arches should PASS"; tail -8 obj/parity/test/t1.log|sed 's/^/    /'; }

# 2) arm red -> FAIL
run_section "2. arm red -> PARITY FAIL"
PARITY_SEQUENTIAL=1 PARITY_ARM_CMD=false PARITY_INTEL_CMD=true \
  bash tools/gate/parity_ci.sh >obj/parity/test/t2.log 2>&1
[ $? -ne 0 ] && pass "arm red -> FAIL" || fail "arm red should FAIL"

# 3) intel red -> FAIL
run_section "3. intel red -> PARITY FAIL"
PARITY_SEQUENTIAL=1 PARITY_ARM_CMD=true PARITY_INTEL_CMD=false \
  bash tools/gate/parity_ci.sh >obj/parity/test/t3.log 2>&1
[ $? -ne 0 ] && pass "intel red -> FAIL" || fail "intel red should FAIL"

# 4) window over budget -> FAIL (DoD)
run_section "4. window over budget -> PARITY FAIL"
PARITY_SEQUENTIAL=1 PARITY_BUDGET_S=1 PARITY_ARM_CMD="sleep 2; true" PARITY_INTEL_CMD=true \
  bash tools/gate/parity_ci.sh >obj/parity/test/t4.log 2>&1
[ $? -ne 0 ] && pass "window over budget (2s > 1s) -> FAIL" || fail "over-budget should FAIL (budget 1s, window 2s)"

# 5) concurrency: parallel window <= sequential window
run_section "5. concurrent window <= sequential window (run window < best)"
SLOW="sleep 2; true"
# window is reported on the stdout of each run; capture it from the logs
PARITY_SEQUENTIAL=0 PARITY_ARM_CMD="$SLOW" PARITY_INTEL_CMD="$SLOW" bash tools/gate/parity_ci.sh >obj/parity/test/t5p.log 2>&1
PARITY_SEQUENTIAL=1 PARITY_ARM_CMD="$SLOW" PARITY_INTEL_CMD="$SLOW" bash tools/gate/parity_ci.sh >obj/parity/test/t5s.log 2>&1
PAR=$(grep -oE 'window: [0-9]+s *budget' obj/parity/test/t5p.log | grep -oE '[0-9]+' | head -1)
SEQd=$(grep -oE 'window: [0-9]+s *budget' obj/parity/test/t5s.log | grep -oE '[0-9]+' | head -1)
echo "  parallel window=${PAR}s sequential window=${SEQd}s"
if [ -n "$PAR" ] && [ -n "$SEQd" ] && [ "$PAR" -le "$SEQd" ]; then
  pass "parallel (${PAR}s) <= sequential (${SEQd}s)"
else
  fail "parallel (${PAR}s) should be <= sequential (${SEQd}s)"
fi

# 6) real wiring: t3 verdict over real logs per arch
run_section "6. real t3-verdict wiring (no QEMU)"
CLEAN="obj/parity/test/pc_clean.log"; DUMMY="obj/parity/test/pc_dummy.log"
{ for i in $(seq 1 1100); do echo "console line $i"; done; echo "System halt"; } > "$CLEAN"
{ for i in $(seq 1 400); do echo "console line $i"; done; echo "SHELLTEST FAIL"; for i in $(seq 401 1100); do echo "console line $i"; done; echo "System halt"; } > "$DUMMY"
PARITY_SEQUENTIAL=1 \
  PARITY_ARM_CMD="bash tools/t3_tier.sh police '$CLEAN' arm" \
  PARITY_INTEL_CMD="bash tools/t3_tier.sh police '$CLEAN' intel" \
  bash tools/gate/parity_ci.sh >obj/parity/test/t6a.log 2>&1
[ $? -eq 0 ] && pass "clean log both arches -> PASS" || fail "clean both arches should PASS"
PARITY_SEQUENTIAL=1 \
  PARITY_ARM_CMD="bash tools/t3_tier.sh police '$CLEAN' arm" \
  PARITY_INTEL_CMD="bash tools/t3_tier.sh police '$DUMMY' intel" \
  bash tools/gate/parity_ci.sh >obj/parity/test/t6b.log 2>&1
[ $? -ne 0 ] && pass "intel over FAIL-injected log -> PARITY FAIL" || fail "FAIL-injected intel should make parity FAIL"

echo
if [ "$fails" = "1" ]; then echo "PARITY_CI_TEST: FAIL"; exit 1; fi
echo "PARITY_CI_TEST: PASS — parity verdict + run-window budget behave correctly"
exit 0
