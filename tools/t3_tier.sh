#!/usr/bin/env bash
# tools/t3_tier.sh — the T3 integration tier AS A GATE (GB-056/GB-057).
#
# `make test` / `make test_intel` used to be `make MODE=test run`: the tier
# booted QEMU, printed whatever the in-OS suites printed, and — because a
# bare `make` recipe only sees the exit status of QEMU — returned 0 even
# when the console was full of failures.  That is how the intel T3 tier
# shipped 222 line-starting `FAIL` lines while reporting green, and how a
# wrong exit-status capture (GB-057) went unnoticed for a whole baseline.
#
# This wrapper makes the tier's verdict depend on its own output:
#   * builds, then runs the wave with a hard wall-clock deadline
#     (the 30-second deadlock rule generalised: a hang is a defect, so a
#     run that never reaches its halt line FAILS the tier, it is never
#     silently re-run);
#   * requires the completion marker ("System halt");
#   * counts FAIL lines in the captured console and FAILS if there is one.
#
# Usage:  tools/t3_tier.sh [arm|intel]     (or: make test / test_intel)
# Env:    T3_TIMEOUT (default 2700 s), T3_LOG (default obj/t3_<arch>.log)
set -euo pipefail

ARCH_ARG="${1:-arm}"
case "$ARCH_ARG" in
  arm)   ARCH=arm ;;
  intel) ARCH=intel ;;
  *) echo "usage: t3_tier.sh [arm|intel]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

T3_TIMEOUT="${T3_TIMEOUT:-2700}"
LOG="${T3_LOG:-$ROOT/obj/t3_${ARCH}.log}"
mkdir -p "$(dirname "$LOG")"

echo "=== T3 ($ARCH): build ==="
make ARCH="$ARCH" MODE=test hobbyos.elf disk.img >"$LOG.build" 2>&1 || {
  echo "T3_${ARCH}: BUILD FAILED"; tail -40 "$LOG.build"; exit 1;
}

echo "=== T3 ($ARCH): run (deadline ${T3_TIMEOUT}s, log $LOG) ==="
: >"$LOG"
# Same QEMU invocation as `make run`, serial captured to the log.  A
# background job + poll (not a bare `timeout make`) so we can distinguish
# "guest halted" from "deadline hit" and report both honestly.
make ARCH="$ARCH" MODE=test run QEMU_ARGS="-display none" >>"$LOG" 2>&1 &
MAKE_PID=$!
cleanup() {
  pkill -P "$MAKE_PID" 2>/dev/null || true
  kill "$MAKE_PID" 2>/dev/null || true
  wait "$MAKE_PID" 2>/dev/null || true
}
trap cleanup EXIT

deadline=$(( $(date +%s) + T3_TIMEOUT ))
halted=0
while kill -0 "$MAKE_PID" 2>/dev/null; do
  if grep -aq "System halt" "$LOG"; then halted=1; break; fi
  if [ "$(date +%s)" -ge "$deadline" ]; then break; fi
  sleep 2
done
# Let QEMU/make wind down once the guest has halted.
for _ in $(seq 1 30); do kill -0 "$MAKE_PID" 2>/dev/null || break; sleep 1; done
cleanup
trap - EXIT

FAIL_LINES=$(grep -ac "FAIL" "$LOG" || true)
echo "T3_${ARCH}: $(wc -l <"$LOG") console lines, ${FAIL_LINES} FAIL lines, halted=${halted}"

rc=0
if [ "$halted" != "1" ]; then
  echo "T3_${ARCH}: FAIL — the wave never reached its halt line within ${T3_TIMEOUT}s (hang = defect, not a flake)"; rc=1
fi
if [ "$FAIL_LINES" != "0" ]; then
  echo "T3_${ARCH}: FAIL — ${FAIL_LINES} FAIL line(s); first 20:"
  grep -an "FAIL" "$LOG" | head -20
  rc=1
fi
[ "$rc" -eq 0 ] && echo "T3_${ARCH}: PASS (0 FAIL lines, halted)"
echo "T3_${ARCH} EXIT=$rc"
exit "$rc"
