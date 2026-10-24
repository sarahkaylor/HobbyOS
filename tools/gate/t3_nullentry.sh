#!/usr/bin/env bash
# tools/gate/t3_nullentry.sh — GB-089 null-entry control gate.
#
# GB-087 root cause (DEEP verifier, a53f26e): the manifest wave loader
# (test_wave_loader, src/kernel/main.c) is entered by iretq onto an empty
# kernel-task stack — NO return address by construction.  When a WAVE.CFG
# manifest dispatches one or more programs, the loader used to `return`, which
# popped address 0 and executed it: #GP vector 13, err 0, rip=0, pid 1, at
# T3_SMP=1, on any manifest wave.  The fix replaces `return` with
# kernel_exit() (terminate, not return).  MR a53f26e.
#
# This gate is the provocation GB-087's fix was blocked on ("nothing can
# provoke the fault it fixes").  It boots a SINGLE-program WAVE.CFG manifest at
# T3_SMP=1 and asserts the STATE-dependent outcome:
#
#   state=unfixed (main, no kernel_exit): the run must FAIL by NAMING the fault
#       -- intel  'vector=13' / 'rip=0x0', ARM 'EL1'/'ELR=0x0' -- and exit
#       NON-ZERO.  If it does not fault, the control is not a control.
#   state=fixed   (a53f26e +): the run must reach 'System halt' (halted=1),
#       exit 0.
#
# DoD last clause: "the control is executed by a make target inside the gate,
# never by hand."  So this script is driven by `make t3_nullentry ARCH=arm|intel
# [STATE=fixed|unfixed]` (State defaults to fixed = the tree's current manifest
# loader, which on main is the unfixed one ONLY in a worktree carrying the
# revert).
#
# Usage:
#   bash tools/gate/t3_nullentry.sh <arch> <fixed|unfixed> [<manifest_program>]
# Env: T3_SMP (default 1 — the fault is single-CPU specific).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${1:-intel}"
STATE="${2:-fixed}"
PROG="${3:-HELLO.BIN}"
SMP="${T3_SMP:-1}"

case "$ARCH" in arm|intel) ;; *) echo "usage: t3_nullentry.sh [arm|intel] [fixed|unfixed]"; exit 2 ;; esac
case "$STATE" in fixed|unfixed) ;; *) echo "state must be fixed|unfixed"; exit 2 ;; esac

# Detect the actual tree state so a mislabeled STATE is reported, not trusted.
# Crucially this must ONLY inspect the test_wave_loader dispatched block — a
# naive `grep kernel_exit src/kernel/main.c` matches the init path's own
# kernel_exit and would false-classify an UNFIXED tree as FIXED.
# The unfixed code is `if (dispatched > 0) return;`, the fixed is `if
# (dispatched > 0) { ... kernel_exit(); }`.  Distinguish by the loader body.
if sed -n '/test_wave_loader/,/static void/p' "$ROOT/src/kernel/main.c" 2>/dev/null | \
     sed -n '/if (dispatched > 0)/,/}/p' | grep -q "kernel_exit"; then
  TREE_FIXED=1
else
  TREE_FIXED=0
fi
[ "$STATE" = fixed ] && [ "$TREE_FIXED" = 1 ] && echo "t3_nullentry: tree has the a53f26e fix (STATE=fixed)"
[ "$STATE" = unfixed ] && [ "$TREE_FIXED" = 0 ] && echo "t3_nullentry: tree is unfixed (STATE=unfixed)"
[ "$STATE" = fixed ] && [ "$TREE_FIXED" = 0 ] && { echo "t3_nullentry: FAIL — STATE=fixed but tree has no kernel_exit in test_wave_loader; run against a53f26e or revert"; exit 2; }
[ "$STATE" = unfixed ] && [ "$TREE_FIXED" = 1 ] && { echo "t3_nullentry: FAIL — STATE=unfixed but tree HAS kernel_exit; run against plain main"; exit 2; }

echo "=== t3_nullentry ($ARCH, SMP=$SMP, program=$PROG, state=$STATE) ==="
LOG="$ROOT/obj/t3_${ARCH}_nullentry.log"
: > "$LOG"
printf '%s\n' "$PROG" > "$ROOT/obj/${ARCH}_nullentry.wave"
STAGE="mcopy -o -i '$ROOT/disk.img' '$ROOT/obj/$ARCH/$PROG' ::$PROG || true; \
mcopy -o -i '$ROOT/disk.img' '$ROOT/obj/${ARCH}_nullentry.wave' ::WAVE.CFG || true"

# The unfixed guest faults and HALTS the CPU but QEMU does not auto-exit (no
# "System halt" shutdown), so a synchronous tier would idle to its full
# deadline.  Run the tier in the background and poll the serial for the
# DECISIVE signal per state, then kill the tier+guest when seen:
#   unfixed -> the NT fault (intel 'vector=13', arm 'EL1'/'ELR=0')
#   fixed   -> "System halt" (halted=1)
(cd "$ROOT" && T3_SMP=$SMP T3_LOG="$LOG" T3_STAGE="$STAGE" \
   T3_TIMEOUT=900 bash tools/t3_tier.sh "$ARCH" > "$ROOT/obj/t3_${ARCH}_nullentry.tier.out" 2>&1) &
TIERPID=$!

DECISIVE=0
for wait in $(seq 1 90); do
  if [ "$STATE" = unfixed ]; then
    if [ "$ARCH" = intel ] && grep -aqE "vector=13|FATAL kernel-mode|halting this CPU|rip=0x0" "$LOG" 2>/dev/null; then DECISIVE=1; break; fi
    if [ "$ARCH" = arm ] && grep -aqE "Synchronous Exception in EL1|ELR=0x0|ELR: 0x0" "$LOG" 2>/dev/null; then DECISIVE=1; break; fi
  else
    grep -aq "System halt" "$LOG" 2>/dev/null && { DECISIVE=1; break; }
  fi
  kill -0 "$TIERPID" 2>/dev/null || break   # tier already returned (or failed early)
  sleep 10
done
[ "$wait" -ge 90 ] && echo "t3_nullentry: WARN — no decisive signal in 900s of polling"

# Tear down the guest+tier so the gate returns promptly.
kill -9 "$TIERPID" 2>/dev/null
for pid in $(pgrep -f "qemu-system" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 2

[ -f "$LOG" ] || { echo "t3_nullentry: FAIL — serial '$LOG' missing"; exit 1; }

HALT=$(grep -ac "System halt" "$LOG")
# fault-name assertions per arch
if [ "$ARCH" = intel ]; then
  FAULT=$(grep -acE "vector=13|FATAL kernel-mode|halting this CPU|rip=0x0" "$LOG")
  FAULT_NAME="vector=13"
else
  FAULT=$(grep -acE "Synchronous Exception in EL1|ELR=0x0|ELR: 0x0|Vector: 13" "$LOG")
  FAULT_NAME="EL1/ELR=0"
fi
echo "  halted=$HALT fault_lines=$FAULT (expecting $FAULT_NAME on unfixed)"

OK=1
if [ "$STATE" = unfixed ]; then
  # no-fix MUST fault (control provokes it) and must NOT halt.
  [ "$FAULT" -ge 1 ] || { echo "t3_nullentry: FAIL — unfixed run did NOT fault (control is not a control)"; OK=0; }
  [ "$HALT" -eq 0 ] || { echo "t3_nullentry: FAIL — unfixed run halted; should have faulted at rip=0"; OK=0; }
  [ "$OK" = 1 ] && { echo "t3_nullentry: PASS — unfixed run provoked the $FAULT_NAME fault (GB-087 confirmed)"; exit 0; }
else
  # fixed MUST reach halted=1 with no fault.
  [ "$HALT" -ge 1 ] || { echo "t3_nullentry: FAIL — fixed run did not halt"; OK=0; }
  [ "$FAULT" -eq 0 ] || { echo "t3_nullentry: FAIL — $FAULT fault lines on a fixed tree"; OK=0; }
  [ "$OK" = 1 ] && { echo "t3_nullentry: PASS — fixed run halted cleanly (a53f26e verified)."; exit 0; }
fi
echo "t3_nullentry: FAIL"
exit 1
