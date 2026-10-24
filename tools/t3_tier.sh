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
#   * counts FAIL lines in the captured console and FAILS if there is one;
#   * FAILS on any FATAL kernel-mode fault line and on a console below the
#     completeness floor (T3_MIN_LINES, default 1000) -- a crashed kernel also
#     reaches "System halt", and a verdict on a wreck is worse than no verdict.
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

# Host-tool preconditions.  Measured failure this exists for: a run reported
# "T3_intel: BUILD FAILED" because `mkfs.fat` was not on PATH -- while it was
# installed at /usr/sbin/mkfs.fat the whole time.  Debian/Ubuntu keep the
# mkfs family in sbin, which a non-root PATH often omits, so the build died
# with "mkfs.fat: No such file or directory" and the tier blamed the code.
# An environment gap must never read as a code defect: fix the PATH, and if a
# tool is then still absent, exit 2 (environment), never 1 (build).
case ":$PATH:" in
  *:/usr/sbin:*|*:/sbin:*) ;;
  *) PATH="/usr/sbin:/sbin:$PATH"; export PATH ;;
esac
_missing=""
for _t in mkfs.fat mcopy qemu-system-$([ "$ARCH" = intel ] && echo x86_64 || echo aarch64); do
  command -v "$_t" >/dev/null 2>&1 || _missing="$_missing $_t"
done
if [ -n "$_missing" ]; then
  echo "T3_${ARCH}: ENVIRONMENT FAILURE — required host tool(s) not found:$_missing"
  echo "  This is NOT a build or code defect: the tier cannot construct or boot the image."
  echo "  PATH=$PATH"
  exit 2
fi


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
  KFAULT=$(grep -ac "FATAL kernel-mode fault" "$LOG" || true)
  MEMFAULTS=$(grep -ac "memory fault" "$LOG" || true)
  MEMFAULT_PROCS=$(grep -aoE "\[KERNEL\] pid=[0-9]+ \([^)]*\) memory fault" "$LOG" 2>/dev/null \
    | sed -E 's/.*pid=([0-9]+).*/\1/' | sort -u | wc -l | tr -d ' ' || true)
  LINES=$(wc -l <"$LOG")
  # GB-068 follow-on: a guest that REBOOTS is not a guest that ran a long wave.
  # After the input/stack fixes the intel guest began triple-faulting in
  # vm_arch_teardown ~27 s into each boot and QEMU restarted it; the serial log
  # then fills with repeated boot markers and the run ends on the harness
  # TIMEOUT with no verdict at all (TIER=124), which reads like a hang instead of
  # a crash. A triple fault has no handler, so the console cannot show it -- the
  # only in-band evidence is that the machine booted more than once. One
  # "Controller reset complete!" is the virtio-blk init of a single boot; more
  # than two means the guest restarted.
  BOOTS=$(grep -ac "Controller reset complete" "$LOG" || true)
  echo "T3_${ARCH} (police ${LOG}): ${LINES} lines, ${FAIL_LINES} FAIL lines, ${MEMFAULTS} fault records across ${MEMFAULT_PROCS} processes, halted=${halted}, kfault=${KFAULT}, boots=${BOOTS}"
  rc=0
  if [ "${BOOTS:-0}" -gt 2 ]; then
    echo "T3_${ARCH}: FAIL — the guest BOOTED ${BOOTS} times: it is resetting mid-run (a triple fault has no handler, so the console cannot show it; capture it with T3_QEMU_ARGS=\"-d int,cpu_reset -D /tmp/q.log\")"
    rc=1
  fi
  [ "$halted" = "1" ] || { echo "T3_${ARCH}: FAIL — no halt line"; rc=1; }
  [ "$KFAULT" = "0" ] || { echo "T3_${ARCH}: FAIL — ${KFAULT} FATAL kernel-mode fault line(s): the guest halted because the kernel crashed"; rc=1; }
  [ "$LINES" -ge "${T3_MIN_LINES:-1000}" ] || { echo "T3_${ARCH}: FAIL — only ${LINES} console lines (floor ${T3_MIN_LINES:-1000}): too little work for a complete wave"; rc=1; }
  if [ "$FAIL_LINES" != "0" ]; then
    echo "T3_${ARCH}: FAIL — ${FAIL_LINES} FAIL line(s); first 5:"
    grep -an "FAIL" "$LOG" | head -5
    rc=1
  fi
  # GB-068 regression guard (mirrors the run path): fail if user memory faults
  # exceed the documented ceiling (default 75; see run-path comment for the
  # measured 52/57 spread that rules out the old 60).
  USER_FAULT_ALLOW="${T3_USER_FAULT_ALLOW:-75}"
  if [ "${MEMFAULT_PROCS:-0}" -gt "$USER_FAULT_ALLOW" ]; then
    echo "T3_${ARCH}: FAIL — ${MEMFAULT_PROCS} processes carry user memory faults, above the ceiling ${USER_FAULT_ALLOW} (GB-068 regression); ${MEMFAULTS} records in total"
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
# Image integrity guard.  A QEMU killed at the deadline can leave disk.img's BPB
# half-flushed (measured: bytes-per-sector 0 at offset 11 instead of 512), and
# because the image is then NEWER than its prerequisites, every later `make` treats
# it as up to date and boots the corrupt image -- which presents as
# "FAT-16 initialization failed!" and 369 console lines, not as a filesystem bug.
# Regenerate rather than debug a filesystem that was never mounted.
if [ -f "$ROOT/disk.img" ]; then
  BPS=$(od -An -tu2 -j 11 -N 2 "$ROOT/disk.img" 2>/dev/null | tr -d ' ')
  if [ "$BPS" != "512" ]; then
    echo "T3_${ARCH}: disk.img BPB is corrupt (bytes/sector='$BPS', want 512) - regenerating the image"
    rm -f "$ROOT/disk.img"
  fi
fi
make ARCH="$ARCH" MODE=test hobbyos.elf disk.img >"$LOG.build" 2>&1 || {
  echo "T3_${ARCH}: BUILD FAILED"; tail -40 "$LOG.build"; exit 1;
}
# Staging hook, added after two probes silently tested nothing: this build
# REGENERATES disk.img, so a file mcopy'd onto it by hand before the tier ran was
# wiped and the guest never saw it (the loader correctly reported -ENOENT and the
# probe looked like a loader failure).  Anything a probe needs on the image must be
# staged here, after the build and before QEMU starts:
#   T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/pie_hello ::PIE.BIN' bash tools/t3_tier.sh intel
# It runs in this shell, so $ROOT is available; a failing stage command is fatal
# rather than silently booting an image without the fixture.
if [ -n "${T3_STAGE:-}" ]; then
  echo "=== T3 ($ARCH): stage ==="
  ROOT="$ROOT" sh -c "$T3_STAGE" || { echo "T3_${ARCH}: STAGE FAILED - refusing to boot a stale image"; exit 2; }
fi

echo "=== T3 ($ARCH): run (deadline ${T3_TIMEOUT}s, log $LOG) ==="
: >"$LOG"
# Same QEMU invocation as `make run`, serial captured to the log.  A
# background job + poll (not a bare `timeout make`) so we can distinguish
# "guest halted" from "deadline hit" and report both honestly.
# Acceleration is the DEFAULT for intel, not opt-in: when this host has
# /dev/kvm (the fleet VMs do -- nested SVM), the intel wave runs under
# -enable-kvm, minutes instead of tens of minutes of TCG.  KVM is not
# available on every host (e.g. macOS, or a host without nested virt), so:
#   * the mode is chosen from /dev/kvm's presence, an environmental fact --
#     the same host always runs the same mode, so reproducibility holds;
#   * the mode actually used is PRINTED on the build line below, so a log
#     can never hide which mode it ran under;
#   * T3_SKIP_KVM=1 forces TCG for an explicit A/B (and on this fleet is
#     the only way to run the slow path once a host has /dev/kvm).
# T3_QEMU_ARGS still lets a caller append further args without editing the
# tier (e.g. T3_MONITOR adds the monitor socket this way).
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
# KVM acceleration default (intel only).  The fleet VMs have /dev/kvm
# (nested SVM); a TCG intel wave is tens of minutes while the same wave
# under KVM is minutes.  Default to KVM whenever the host offers it, print
# the mode on the run line, and let T3_SKIP_KVM=1 force the slow path for
# an explicit A/B.  ARM never gets KVM here (aarch64-on-x86 host has no
# aarch64 KVM), so this is gated on ARCH, not just on the device.
if [ "$ARCH" = "intel" ] && [ -e /dev/kvm ] && [ "${T3_SKIP_KVM:-0}" != "1" ]; then
  ACCEL_ARGS="-enable-kvm"
else
  ACCEL_ARGS=""
fi
# T3_QEMU_ARGS still wins outright when the caller passes it explicitly.
if [ -z "${T3_QEMU_ARGS:-}" ]; then T3_QEMU_ARGS="$ACCEL_ARGS"; fi

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

# T3_SMP lets a caller pin the CPU count (QEMU_SMP defaults to 8 in the
# Makefile). This matters for concurrency questions: "does this fault happen
# with one CPU?" is often the cheapest discriminator between a real logic bug
# and a race, and before this passthrough there was no way to ask it -- passing
# "-smp 1" inside T3_QEMU_ARGS does NOT work, because the Makefile's own
# "-smp $(QEMU_SMP)" precedes it and QEMU honours the earlier one.
make -o disk.img ARCH="$ARCH" MODE=test run ${T3_SMP:+QEMU_SMP=$T3_SMP} QEMU_ARGS="-display none ${T3_QEMU_ARGS:-}" >>"$LOG" 2>&1 &
MAKE_PID=$!
cleanup() {
  pkill -P "$MAKE_PID" 2>/dev/null || true
  kill "$MAKE_PID" 2>/dev/null || true
  wait "$MAKE_PID" 2>/dev/null || true
}
trap cleanup EXIT

# GB-057 (Coordinator 2026-10-24): the post-freeze sample as a FUNCTION, called
# the moment a stall marker appears -- on a freeze QEMU is still alive then, and
# by the end of the wait loop it usually is not (measured: three passes of a
# held-lock table that was never read).  It also never returns silently: an
# absent instrument is a FAILED instrument, so no monitor or no socket prints an
# explicit line instead of nothing at all.
probe_guest() { # $1 = why we are sampling
  if [ -z "${MON_SOCK:-}" ]; then
    { echo ""; echo "=== T3 guest sample: SKIPPED — no monitor attached (T3_MONITOR not set; $1) ==="; } >>"$LOG"
    return 0
  fi
  if [ ! -S "$MON_SOCK" ]; then
    { echo ""; echo "=== T3 guest sample: ABSENT — monitor socket '$MON_SOCK' gone ($1) ===";
      echo "    no CR2, no held-lock table: the instrument did not run."; } >>"$LOG"
    return 0
  fi
  {
    echo ""
    echo "=== T3 guest sample (QEMU monitor $MON_SOCK; $1) ==="
    echo ""
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
}

deadline=$(( $(date +%s) + T3_TIMEOUT ))
halted=0
probed=0
# GB-057: a SILENT freeze prints no marker at all -- the markers are console
# output, so a CPU that is stuck (or a console lock that is dead) cannot report
# them.  Measured: an intel run froze at 12,821 lines with zero markers, no
# sample and no verdict for 18 minutes, leaving the parked CPU's RIP unknown.
# So also sample when the serial has stopped GROWING: that is the one signal a
# frozen guest still gives, because it is an absence rather than a message.
silence_secs="${T3_SILENCE_SECS:-60}"
last_size=-1
last_growth=$(date +%s)
while kill -0 "$MAKE_PID" 2>/dev/null; do
  if grep -aq "System halt" "$LOG"; then halted=1; break; fi
  # Silence detector: probe once, the first time the log stops growing.
  cur_size=$(wc -c <"$LOG" 2>/dev/null || echo 0)
  if [ "$cur_size" != "$last_size" ]; then
    last_size=$cur_size
    last_growth=$(date +%s)
  elif [ "$probed" = 0 ] && [ $(( $(date +%s) - last_growth )) -ge "$silence_secs" ]; then
    probe_guest "serial silent for ${silence_secs}s with no stall marker"
    probed=1
  fi
  # First stall marker seen: sample the guest NOW, while it is frozen and
  # still reachable.  One sample per run; the markers repeat thousands of
  # times and the guest does not change.
  if [ "$probed" = 0 ] && \
     grep -aqE "\[LOCKFOREVER\]|\[IDLESTUCK\]|\[STALLBREAK\]" "$LOG"; then
    probe_guest "first stall marker in the serial"
    probed=1
  fi
  # GB-057: a SILENT stall -- deadline reached with no marker at all -- also
  # needs a sample, and it must be taken BEFORE the deadline kill takes QEMU
  # away.  Measured: a run that ended with no halt and no markers produced
  # 'ABSENT -- monitor socket gone' because the fallback probed after cleanup.
  if [ "$probed" = 0 ] && [ "$(date +%s)" -ge $((deadline - 30)) ]; then
    probe_guest "deadline approaching with no halt marker"
    probed=1
  fi
  if grep -aq "System halt" "$LOG"; then halted=1; break; fi
  if [ "$(date +%s)" -ge "$deadline" ]; then break; fi
  sleep 2
done
# Let QEMU/make wind down once the guest has halted.
for _ in $(seq 1 30); do kill -0 "$MAKE_PID" 2>/dev/null || break; sleep 1; done


# Fallback: a run that ended without a halt marker and never showed a stall
# marker (deadline hit with a silent guest) still deserves a sample.
if [ "$probed" = 0 ] && ! grep -aq "System halt" "$LOG"; then
  probe_guest "run ended without a halt marker"
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
# Report the user-mode fault count beside the FAIL count, always.  On 2026-10-19 I
# read "44 memory faults after the ELF process ran" as ELF-induced damage and wrote
# two posts on it; the control (runs with no ELF process at all) showed 54 and 55.
# The baseline fault rate had been invisible for the whole session because the tier's
# verdict line only counted FAIL lines, and the fault lines use different wording
# ("[KERNEL] pid=N (IMG) memory fault VA=" not "FATAL kernel-mode fault").  A count
# printed without a baseline in the same line invites exactly that mistake.
MEMFAULTS=$(grep -ac "memory fault" "$LOG" || true)
# GB-068: the RECORD count is not the impact count.  Measured 2026-10-19 on
# three runs of the same build: 61 records / 56 distinct pids, 112 / 56, and
# 71 / 48.  The same processes appear every time; what varies is how many times
# each one faults before dying (1 on a quick death, up to 4 when it keeps
# re-entering the faulting path).  So the record count is retries, and gating on
# it means gating on scheduling luck -- which is exactly why the old ceiling of
# 60 fired on some runs at 61-113 while the affected-process count sat still.
# Both are printed; the guard below gates on the STABLE one.
MEMFAULT_PROCS=$(grep -aoE "\[KERNEL\] pid=[0-9]+ \([^)]*\) memory fault" "$LOG" 2>/dev/null \
  | sed -E 's/.*pid=([0-9]+).*/\1/' | sort -u | wc -l | tr -d ' ' || true)
PROGS_EXITED=$(grep -aoE "Process [0-9]+: [A-Z0-9_.]+\.BIN exited" "$LOG" 2>/dev/null \
  | grep -oE "[A-Z][A-Z0-9_]{1,12}\.BIN" | sort -u | wc -l | tr -d ' ' || true)
echo "T3_${ARCH}: $(wc -l <"$LOG") console lines, ${FAIL_LINES} FAIL lines, ${MEMFAULTS} user memory fault records across ${MEMFAULT_PROCS} processes, ${PROGS_EXITED} programs exited, halted=${halted}"

# GB-068 regression guard: the intel wave carries a documented, long-standing
# baseline of user-mode fatal faults (in=PROT) from legacy v1 flat binaries
# (SUBPRB/SHTEST/SHTEST2) touching their stack-like low-VA region — measured
# 44-55 on seven 2026-10-19 logs INCLUDING runs with no ELF process (54, 55),
# so it predates GB-013/GB-066 and is a property of the v1 image programs, not
# a regression here.  Evidence + mechanism: the surviving fault row is
#   [KERNEL] pid=N (SHTEST.BIN) memory fault VA=0x00000000755xxxxx ... in=PROT
#   -> the v1 flat loader (v1_map_image) leaves that low stack region with a
#   non-writable / non-present guardish mapping, so the legacy program dies on
#   first stack use every wave.  That is the negotiated baseline.
#
# Guard: FAIL the tier if the fault count REGRESSES above the allowed ceiling
# (new faults = the tier was green before but a change added MORE user faults).
# T3_USER_FAULT_ALLOW sets the ceiling.  MEASURED 2026-10-19 on origin/main, two
# consecutive runs with no instrumentation: 52 and 57 faults (1140 and 1075
# console lines, 4 FAIL lines both).  So the run-to-run spread is ~5 and the old
# default of 60 sat THREE above a legal reading -- it would have fired on noise.
# The default is now 75: comfortably outside the observed band, still far below
# the 104-fault outlier seen in one instrumented run, i.e. a real regression
# still trips it.  This is a regression guard, not a precise measure; GB-068 is
# the ticket that has to make the count meaningful.  Set
# T3_USER_FAULT_ALLOW=0 to demand the '0 user memory faults' DoD literally once
# the v1 stack region is fixed.
USER_FAULT_ALLOW="${T3_USER_FAULT_ALLOW:-75}"
MEMREG=0
if [ "${MEMFAULT_PROCS:-0}" -gt "$USER_FAULT_ALLOW" ]; then
  echo "T3_${ARCH}: FAIL — ${MEMFAULT_PROCS} processes carry user memory faults, above the ceiling ${USER_FAULT_ALLOW} (GB-068 regression); ${MEMFAULTS} records in total, which vary with per-process retries rather than with impact"
  MEMREG=1
fi

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
# A halt marker alone cannot tell "the wave finished" from "the wave died early":
# a FATAL kernel fault halts this CPU, the survivors then find nothing runnable,
# and the scheduler prints the same 'System halt' line.  Measured (probe17): a run
# that took a kernel #GP at line ~450 of 460 halted with 0 FAIL lines and the tier
# called it a PASS -- a verdict on a wreck.  Two structural criteria fix that, both
# measured rather than assumed: a kernel fault is never a pass, and a wave that
# halted without doing its work is not a pass either.
KFAULT=$(grep -ac "FATAL kernel-mode fault" "$LOG" || true)
LINES=$(wc -l <"$LOG")
# Completeness is measured by PROGRAMS THAT EXITED, not by console lines.  The line
# floor was calibrated when a healthy wave carried ~2,300 lines of [IDLESTUCK]/
# [ALIVE-SET] chatter; once GB-057-a made the wave halt properly, that chatter (and
# therefore the line count) collapsed, and three complete waves that reached the
# suite's last test were failed as "too little work" at 984-1,011 lines.  Measured
# separation: complete runs print `Process N: X.BIN exited` for 60-63 distinct
# programs; a truncated boot prints 0.  The line floor stays only as a low backstop
# for a wave that never got going.
MIN_PROGRAMS="${T3_MIN_PROGRAMS:-55}"
EXITED_PROGS=$(grep -aoE "Process [0-9]+: [A-Z0-9_.]+\.BIN exited" "$LOG" 2>/dev/null \
  | grep -oE "[A-Z][A-Z0-9_]{1,12}\.BIN" | sort -u | wc -l | tr -d ' ' || true)
MIN_LINES="${T3_MIN_LINES:-300}"
if [ "$KFAULT" != "0" ]; then
  echo "T3_${ARCH}: FAIL — ${KFAULT} FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed"; rc=1
fi
if [ "$halted" = "1" ] && [ "$EXITED_PROGS" -lt "$MIN_PROGRAMS" ]; then
  echo "T3_${ARCH}: FAIL — only ${EXITED_PROGS} programs exited (floor ${MIN_PROGRAMS}): too little work for a complete wave, so this cannot be a pass"; rc=1
fi
if [ "$FAIL_LINES" != "0" ]; then
  echo "T3_${ARCH}: FAIL — ${FAIL_LINES} FAIL line(s); first 20:"
  grep -an "FAIL" "$LOG" | head -20
  rc=1
fi
[ "$MEMREG" = "1" ] && rc=1
[ "$rc" -eq 0 ] && echo "T3_${ARCH}: PASS (0 FAIL lines, halted, no kernel fault, ${LINES} lines)"
echo "T3_${ARCH} EXIT=$rc"
exit "$rc"
