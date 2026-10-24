#!/usr/bin/env bash
# tools/gate/t3_pollbrk.sh — GB-086 clause-3 gate: the broken-select control
# POLLBRK.BIN must boot, run, and FAIL the select-timeout check.
#
# GB-086 DoD clause 3: "a deliberately-broken select (returning immediately)
# still FAILs the check at both CPU counts".  The control (built with
# -DPOLLTST_BROKEN_SELECT, a zero-timeout select that returns ~0ms) must be
# booted and its FULL result asserted: the guest reaches "System halt"
# (halted=1) AND the log contains the failure row
#     POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL
#     POLLTST elapsed_ms=0
# and POLLTST FAILED: 1 check(s).  A control that PASSES (or a guest that
# crashes before reaching the check) is a gate FAILURE — the gate proves the
# 'gate cannot be painted green by a broken select' property.
#
# "Measured manually is not a gate" (Coordinator, 2026-10-24): this script is
# the target.  It drives the REAL tier (tools/t3_tier.sh) with a POLLBRK-only
# manifest via T3_STAGE, then asserts on the captured serial.
#
# Usage:
#   bash tools/gate/t3_pollbrk.sh [arm|intel] [--expect-fail|--debug]
# Env:
#   T3_SMP      CPU count for BOTH the control run and... (default 8)
#
# Exit 0 = control correctly FAILed at the given arch+count.
# Exit 1 = gate FAILURE (control passed / kernel crashed / row absent).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${1:-arm}"
EXPECT="${2:-fail}"
SMP="${T3_SMP:-8}"

case "$ARCH" in arm|intel) ;; *) echo "usage: t3_pollbrk.sh [arm|intel]"; exit 2 ;; esac
[ "$EXPECT" = fail ] || [ "$EXPECT" = pass ] || { echo "expect must be fail|pass"; exit 2; }

# --- stage: build control, then boot with a POLLBRK-only manifest -------------
echo "=== t3_pollbrk ($ARCH, T3_SMP=$SMP): build + boot broken-select control ==="

# A manifest-only wave lacks the >1000-line completeness floor, so we capture
# the serial with a custom T3_LOG and drive the tier with T3_SKIP_HALT_CHECK
# only if needed — but we still assert halted=1 ourselves below.  We override
# T3_LOG to a per-arch file so concurrent gate runs don't clobber.
LOG="$ROOT/obj/t3_${ARCH}_pollbrk.log"
: > "$LOG"

# Boot the tier with the POLLBRK manifest staged AFTER the build (T3_STAGE
# runs after disk.img regeneration).  The tier's completeness-floor FAIL for a
# single-program wave is expected and not inspected; we only read the serial.
STAGE="mcopy -o -i '$ROOT/disk.img' '$ROOT/obj/$ARCH/POLLBRK.BIN' ::POLLBRK.BIN; \
printf 'POLLBRK.BIN\\n' > '$ROOT/obj/$ARCH/pollbrk.wave'; \
mcopy -o -i '$ROOT/disk.img' '$ROOT/obj/$ARCH/pollbrk.wave' ::WAVE.CFG"

(cd "$ROOT" && T3_SMP=$SMP T3_LOG="$LOG" T3_STAGE="$STAGE" \
   T3_TIMEOUT=1200 bash tools/t3_tier.sh "$ARCH" > "$ROOT/obj/t3_${ARCH}_pollbrk.tier.out" 2>&1)
TIER_RC=$?

echo "=== tier exited $TIER_RC (its wave-floor FAIL for a 1-program manifest is expected) ==="
[ -f "$LOG" ] || { echo "t3_pollbrk: FAIL — serial '$LOG' missing"; exit 1; }

# --- assert: control shipped, ran, and FAILed, guest halted ---------------------
HALTED=$(grep -ac "System halt" "$LOG")
LOADED=$(grep -ac "\[ARRIVED\] POLLBRK.BIN rc>0" "$LOG")
TIMEOUT_FAIL=$(grep -ac "timeout took >= 85ms and < 5000ms (elapsed seen): FAIL" "$LOG")
ZERO=$(grep -ac "elapsed_ms=0" "$LOG")
FAILED=$(grep -ac "POLLTST FAILED: 1 check(s)" "$LOG")
KFATAL=$(grep -ac "FATAL kernel-mode\|Synchronous Exception in EL1\|halting this CPU" "$LOG")

echo "  halted=$HALTED loaded=$LOADED timeout_FAIL=$TIMEOUT_FAIL elapsed0=$ZERO failed=$FAILED kfatal=$KFATAL"
echo "  sample:"
grep -aA2 "timeout took" "$LOG" | head -4

OK=1
[ "$HALTED" -ge 1 ]  || { echo "t3_pollbrk: FAIL — guest did not halt"; OK=0; }
[ "$LOADED" -ge 1 ]  || { echo "t3_pollbrk: FAIL — POLLBRK.BIN did not load/ship"; OK=0; }
if [ "$EXPECT" = fail ]; then
  [ "$TIMEOUT_FAIL" -ge 1 ] || { echo "t3_pollbrk: FAIL — control did NOT fail the timeout check (gate paintable green!)"; OK=0; }
  [ "$ZERO" -ge 1 ]        || { echo "t3_pollbrk: FAIL — no elapsed_ms=0 row"; OK=0; }
  [ "$FAILED" -ge 1 ]      || { echo "t3_pollbrk: FAIL — POLLTST did not report FAILED"; OK=0; }
  [ "$KFATAL" -eq 0 ]      || { echo "t3_pollbrk: FAIL — kernel faulted (GB-087?) instead of the control FAILing"; OK=0; }
else
  # --expect-pass: the healthy (non-broken) POLLTST must PASS the timeout check
  PS=$(grep -ac "timeout took >= 85ms and < 5000ms (elapsed seen): PASS" "$LOG")
  [ "$PS" -ge 1 ] && [ "$TIMEOUT_FAIL" -eq 0 ] || { echo "t3_pollbrk: FAIL — expected PASS, got FAIL"; OK=0; }
fi

if [ "$OK" = "1" ]; then
  echo "t3_pollbrk: PASS — broken-select control FAILed the >=85ms check (gate not paintable green)."
  exit 0
fi
echo "t3_pollbrk: FAIL"
exit 1
