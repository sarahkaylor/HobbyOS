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
POLICE_ONLY=""
if [ "$ARCH_ARG" = "police" ]; then
  # tools/t3_tier.sh police <log> — run the verdict logic over an ALREADY
  # captured console log.  This is how the tier's own FAIL detector is
  # proven (inject a FAIL line into a green log: the tier must go red).
  POLICE_ONLY="${2:?usage: t3_tier.sh police <log>}"
  ARCH_ARG="${3:-intel}"
fi
case "$ARCH_ARG" in
  arm)   ARCH=arm ;;
  intel) ARCH=intel ;;
  *) echo "usage: t3_tier.sh [arm|intel] | t3_tier.sh police <log> [arch]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ -n "$POLICE_ONLY" ]; then
  LOG="$POLICE_ONLY"
  # A log whose guest never booted must not be judged as a hang (see the run
  # path's guard): "a hang is a defect" only holds while an environment failure
  # cannot wear its clothes.
  if grep -aq 'Failed to get "write" lock' "$LOG"; then
    echo "T3_${ARCH} (police ${LOG}): ENVIRONMENT FAILURE — disk.img was locked; the guest never booted"
    echo "T3_${ARCH} EXIT=2 (environment, not a tier result)"
    exit 2
  fi
  halted=0; grep -aq "System halt" "$LOG" && halted=1
  if [ "${T3_SKIP_HALT_CHECK:-0}" = "1" ]; then halted=1; fi
  FAIL_LINES=$(grep -ac "FAIL" "$LOG" || true)
  echo "T3_${ARCH} (police ${LOG}): $(wc -l <"$LOG") lines, ${FAIL_LINES} FAIL lines, halted=${halted}"
  rc=0
  [ "$halted" = "1" ] || { echo "T3_${ARCH}: FAIL — no halt line"; rc=1; }
  if [ "$FAIL_LINES" != "0" ]; then
    echo "T3_${ARCH}: FAIL — ${FAIL_LINES} FAIL line(s); first 5:"
    grep -an "FAIL" "$LOG" | head -5
    rc=1
  fi
  [ "$rc" -eq 0 ] && echo "T3_${ARCH}: PASS (0 FAIL lines, halted)"
  echo "T3_${ARCH} EXIT=$rc"
  exit "$rc"
fi

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
# T3_QEMU_ARGS lets a caller add acceleration without editing the tier:
#   T3_QEMU_ARGS="-enable-kvm" make test_intel
# runs the intel wave near-native instead of under TCG (the fleet's VMs have
# nested SVM and /dev/kvm, and a TCG 8-vCPU wave takes tens of minutes while
# the same wave under KVM is minutes).  Never hardcoded here: KVM is not
# available on every host, and a tier that silently changes its execution
# mode is not reproducible.
# Post-freeze evidence: read the LIVE guest through QEMU's monitor before
# cleanup kills it.
#
# Why this exists, measured: on GB-057 the guest could not report anything at
# all -- every CPU was interrupt-dead, so no timer fired anywhere and the
# console simply stopped.  Every in-guest instrument was blind by construction.
# The only observer that worked was QEMU itself, asked from outside, and it
# turned an unexplained freeze into a named fault (vector=6 #UD at RIP=0x7 with
# proc_lock held).  A no-halt failure must therefore always leave that evidence
# behind, instead of depending on a human remembering to attach a monitor.
#
# Opt-in via T3_MONITOR=1: it changes the QEMU command line, and a tier that
# silently changes its execution mode is not reproducible.
MON_SOCK=""
if [ "${T3_MONITOR:-0}" = "1" ]; then
  MON_SOCK="${T3_MONITOR_SOCK:-/tmp/t3_monitor_${ARCH}_$$.sock}"
  T3_QEMU_ARGS="${T3_QEMU_ARGS:-} -monitor unix:${MON_SOCK},server,nowait"
fi

# Guard: another QEMU holding disk.img makes this run fail at QEMU start with
# "Failed to get write lock" -- which the verdict below would then report as a
# HANG, because a guest that never booted also never prints 'System halt'.
# Observed on GB-057: a second run started while a frozen guest was still alive
# produced a 293-line "no halt" verdict for a machine that never started.  A
# false hang verdict is worse than no verdict, since "a hang is a defect" is only
# useful while an environment failure cannot masquerade as one.  Refuse to start,
# with a distinct exit code, and say why.
if command -v fuser >/dev/null 2>&1 && fuser "$ROOT/disk.img" >/dev/null 2>&1; then
  echo "T3_${ARCH}: REFUSING TO START — $ROOT/disk.img is in use by another QEMU."
  echo "T3_${ARCH}: a stale frozen guest must be killed first (its monitor socket is usually /tmp/t3_monitor_*.sock)"
  echo "T3_${ARCH} EXIT=2 (environment, not a tier result)"
  exit 2
fi

make ARCH="$ARCH" MODE=test run QEMU_ARGS="-display none ${T3_QEMU_ARGS:-}" >>"$LOG" 2>&1 &
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

# If the console never reached its halt marker while a monitor was attached, read
# the live guest NOW -- cleanup below kills it, and on a freeze this is the only
# evidence that will ever exist.
if [ -n "$MON_SOCK" ] && [ -S "$MON_SOCK" ] && ! grep -aq "System halt" "$LOG"; then
  {
    echo ""
    echo "=== T3 post-freeze guest sample (QEMU monitor $MON_SOCK) ==="
    echo "--- all CPUs (control registers included: CR2 names the fault address) ---"
    printf 'info registers -a\n' | timeout 20 socat - "UNIX-CONNECT:$MON_SOCK" 2>/dev/null \
      | tr -d '\r' | grep -av '^(qemu)' || echo "(monitor read failed)"
    ELF="obj/$ARCH/hobbyos.elf"
    [ -f "$ELF" ] || ELF="hobbyos.elf"
    NMB="$(command -v llvm-nm || command -v nm || true)"
    if [ -n "$NMB" ] && [ -f "$ELF" ]; then
      sym() { "$NMB" --defined-only "$ELF" 2>/dev/null | awk -v s="$1" '$0 ~ " "s"$" {print "0x"$1; exit}'; }
      MON_READ() { printf "%s\n" "$1" | timeout 20 socat - "UNIX-CONNECT:$MON_SOCK" 2>/dev/null | tr -d '\r' | grep -a ':' || true; }
      PL="$(sym proc_lock)"
      [ -n "$PL" ] && { echo "--- proc_lock word (1 = held) ---"; MON_READ "x/1xw $PL"; } || true
      HL="$(sym held_locks)"
      PC="$(sym held_lock_pc)"
      ST="$(sym sched_stage)"
      if [ -n "$HL" ]; then
        echo "--- held-lock table (cpu0..7 x 8 slots): who claims what ---"
        MON_READ "x/64xg $HL"
        [ -n "$PC" ] && { echo "--- acquire sites for those claims ---"; MON_READ "x/64xg $PC"; } || true
        [ -n "$ST" ] && { echo "--- sched_stage per cpu ---"; MON_READ "x/8xb $ST"; } || true
      else
        echo "--- no held-lock table in this build (probe scaffolding absent) ---"
      fi
    fi
    echo "=== end guest sample ==="
  } >>"$LOG" 2>&1
fi

cleanup
trap - EXIT

# Re-read the log: the guest can halt and make can exit between two polls, so
# the loop above may leave `halted` at 0 for a run that DID halt (observed on
# the first ARM run — the tier must never fail a run for that).
if [ "$halted" != "1" ] && grep -aq "System halt" "$LOG"; then
  halted=1
  echo "T3_${ARCH}: note — halt marker found after make exited (poll raced the exit)"
fi

FAIL_LINES=$(grep -ac "FAIL" "$LOG" || true)
echo "T3_${ARCH}: $(wc -l <"$LOG") console lines, ${FAIL_LINES} FAIL lines, halted=${halted}"

# An environment failure must never be reported as a hang (see the pre-flight
# guard above): if QEMU never got the image, this run says nothing about the
# kernel, so it gets its own exit code and its own sentence.
if grep -aq 'Failed to get "write" lock' "$LOG"; then
  echo "T3_${ARCH}: ENVIRONMENT FAILURE — disk.img was locked by another QEMU; this guest never booted, so this is NOT a hang verdict"
  echo "T3_${ARCH} EXIT=2 (environment, not a tier result)"
  exit 2
fi

rc=0
if [ "$halted" != "1" ]; then
  echo "T3_${ARCH}: FAIL — the console has no 'System halt' line (idle/hung wave, or the ${T3_TIMEOUT}s deadline hit; a hang is a defect, not a flake)"; rc=1
fi
if [ "$FAIL_LINES" != "0" ]; then
  echo "T3_${ARCH}: FAIL — ${FAIL_LINES} FAIL line(s); first 20:"
  grep -an "FAIL" "$LOG" | head -20
  rc=1
fi
[ "$rc" -eq 0 ] && echo "T3_${ARCH}: PASS (0 FAIL lines, halted)"
echo "T3_${ARCH} EXIT=$rc"
exit "$rc"
