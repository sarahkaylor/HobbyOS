#!/usr/bin/env bash
# tools/gate/parity_ci.sh — two-arch parity CI (GB-044).
#
# Runs a per-arch check on ARCH=arm AND ARCH=intel and requires BOTH to be
# green before a merge — the two-arch parity rule (gaming.md §2.3).  It also
# enforces the GB-044 DoD "run window < best": the two arches run concurrently
# to bound wall-clock, and a PARITY_BUDGET_S (the best-known baseline window)
# makes the CI FAIL when the measured window exceeds it, rather than silently
# slowing.
#
# Per-arch runner (default: the merge gate, tools/gate/merge_gate.sh, once
# GB-005 lands; otherwise a caller supplies commands):
#   PARITY_ARM_CMD / PARITY_INTEL_CMD   command strings, run with `bash -c`
#   (e.g. "bash tools/t3_tier.sh police <log> arm" for a no-QEMU verdict, or a
#    stub like "true" / "false" for logic tests)
# GATE_T1/T2/T3 and GATE_CI are forwarded to the merge gate when used.
#
# Env:
#   PARITY_BUDGET_S     best-known window baseline; >0 enforces the DoD
#                       (fails when the measured window exceeds it); default 0
#   PARITY_SEQUENTIAL   =1 run the arches one after the other (a single host,
#                       avoids the shared obj//.arch-stamp race); default 0
#                       (concurrent — how the farm bounds the window)
#
# Emits per-arch verdicts, the measured window, and a single PARITY verdict.
# Exit 0 = both arches green and (if a budget is set) window <= budget.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p obj/parity

SEQUENTIAL="${PARITY_SEQUENTIAL:-0}"
BUDGET="${PARITY_BUDGET_S:-0}"

# --- per-arch runner -------------------------------------------------------
DEFAULT_GATE=""
if [ -x tools/gate/merge_gate.sh ]; then DEFAULT_GATE="bash tools/gate/merge_gate.sh"; fi
PARITY_ARM_CMD="${PARITY_ARM_CMD:-$DEFAULT_GATE}"
PARITY_INTEL_CMD="${PARITY_INTEL_CMD:-$DEFAULT_GATE}"
if [ -z "$PARITY_ARM_CMD" ] || [ -z "$PARITY_INTEL_CMD" ]; then
  echo "PARITY: no merge gate on this tree (land GB-005 first) and no PARITY_ARM_CMD/PARITY_INTEL_CMD supplied — refusing rather than silently passing."
  exit 2
fi
export GATE_T1 GATE_T2 GATE_T3 GATE_CI

START=$(date +%s)
run_arch() {  # run_arch <name> <cmd> <logbase>
  local name="$1" cmd="$2" base="$3"
  local exitf="obj/parity/${base}.exit"
  { 
    bash -c "$cmd"
    echo "$?" > "$exitf"
  } > "obj/parity/${base}.log" 2>&1 &
  if [ "$SEQUENTIAL" = "1" ]; then wait; fi
}
run_arch ARM   "$PARITY_ARM_CMD"   arm
run_arch INTEL "$PARITY_INTEL_CMD" intel
wait

END=$(date +%s); WINDOW=$((END-START))
ARM_RC=0;  [ -f obj/parity/arm.exit ]   && ARM_RC=$(cat obj/parity/arm.exit)
INTEL_RC=0; [ -f obj/parity/intel.exit ] && INTEL_RC=$(cat obj/parity/intel.exit)

echo "=== PARITY SUMMARY ==="
echo "  window: ${WINDOW}s   budget: ${BUDGET}s (0 = unbounded)"
echo "  arm   exit: $ARM_RC     ($( [ $ARM_RC -eq 0 ] && echo GREEN || echo RED ))"
echo "  intel exit: $INTEL_RC   ($( [ $INTEL_RC -eq 0 ] && echo GREEN || echo RED ))"
echo "  sequential: $SEQUENTIAL"

fail=0
[ "$ARM_RC" -eq 0 ]   || { echo "  PARITY: arm is RED (log obj/parity/arm.log)"; fail=1; }
[ "$INTEL_RC" -eq 0 ] || { echo "  PARITY: intel is RED (log obj/parity/intel.log)"; fail=1; }
if [ "$BUDGET" -gt 0 ] && [ "$WINDOW" -gt "$BUDGET" ]; then
  echo "  PARITY: window ${WINDOW}s exceeds budget ${BUDGET}s (DoD 'run window < best' violated)"
  fail=1
fi

echo
if [ "$fail" = "1" ]; then
  echo "PARITY: FAIL (two-arch parity: both arches must be green and within budget)"
  exit 1
fi
if [ "$BUDGET" -gt 0 ]; then
  echo "PARITY: PASS — both arches green, window ${WINDOW}s <= budget ${BUDGET}s"
else
  echo "PARITY: PASS — both arches green (window ${WINDOW}s; no budget set)"
fi
exit 0
