#!/usr/bin/env bash
# tools/gate/m2_tp.sh — M2 gate (c): Mono's ThreadPool parks, wakes and runs
# work items, and the timed-semaphore clock contract holds.
#
# WHY THIS GATE EXISTS (L43): the game stranded in the loading screen on a
# managed wait whose signaller was a ThreadPool work item.  Two defects made
# that look like "Mono's ThreadPool is dead":
#
#   1. `gettimeofday(2)` answered UPTIME while `clock_gettime(CLOCK_REALTIME)`
#      answered the RTC epoch.  Mono's mono_os_sem_timedwait() builds an
#      ABSOLUTE deadline as gettimeofday() + timeout_ms and passes it to
#      sem_timedwait(); glibc turns that into
#      futex(FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME), and the kernel compared
#      it against the realtime clock -- so every 5..60 s threadpool park
#      expired 56 years early and the worker exited without ever parking.
#      Row: sem_mono_deadline elapsed_ms=0 (guest) vs 50 (host).
#   2. The reproducer itself was run under the OFFICIAL mono-sgen, which is a
#      monolithic build that hides four mono_* symbols libmono-native.so
#      imports.  System.Native therefore cannot load, EVERY threadpool worker
#      dies in ThreadPoolWorkQueueThreadLocals..ctor -> new Random(), and the
#      probe reports "TP TIMEOUT parallel done=0" on any kernel -- the host
#      included.  A passing row from that binary proves nothing.
#
# So this gate runs the probe under the GAME's own runtime
# (MonoBleedingEdge/libmonobdwgc-2.0.so) through a host/guest-portable
# launcher, and grades the managed rows the probe prints.
#
# Usage: tools/gate/m2_tp.sh [--timeout S]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
TIMEOUT_S=180
[ "${1:-}" = "--timeout" ] && TIMEOUT_S="$2"
OUT="$ROOT/obj/tp-probe"
LOG="$OUT/serial-tp-gate.log"
mkdir -p "$OUT"

echo "=== m2_tp: build the reproducer (Roslyn against Mono's own mscorlib) ==="
CSC="$(ls -d /usr/lib/dotnet/sdk/*/Roslyn/bincore/csc.dll 2>/dev/null | head -1)"
[ -n "$CSC" ] || { echo "m2_tp: no Roslyn csc.dll (install the .NET SDK)"; exit 2; }
MSCORLIB=/srv/nfs/rimworld/RimWorldLinux_Data/Managed/mscorlib.dll
[ -f "$MSCORLIB" ] || { echo "m2_tp: no $MSCORLIB"; exit 2; }
dotnet "$CSC" -nologo -nostdlib -r:"$MSCORLIB" \
  -out:"$OUT/tp.exe" tools/rw/mono-probe/tp.cs || { echo "m2_tp: csc FAILED"; exit 2; }

echo "=== m2_tp: 1/2 ThreadPool work items run and workers park (guest) ==="
# --tptrace: Mono's own threadpool trace, so the gate can assert that a worker
# actually PARKED (the gettimeofday defect made worker_park() return "timed out"
# on the first futex call: 0 parking rows and a worker that exits instead).
bash tools/rw/run-tp-probe.sh --timeout "$TIMEOUT_S" --tptrace --log serial-tp-gate.log \
  > "$OUT/gate-run.log" 2>&1
grep -a 'TP \|TP-OK' "$LOG" | sed 's/^/  /'

PAR=$(grep -ac 'TP OK parallel done=8' "$LOG")
JOIN=$(grep -ac 'TP OK join v=7' "$LOG")
DONE=$(grep -ac '^TP-OK' "$LOG")
TIMEOUTS=$(grep -ac 'TP TIMEOUT' "$LOG")
PARK=$(grep -ac 'worker parking' "$LOG")
WPARK=$(grep -ac 'worker unparking' "$LOG")
echo "  rows: TP OK parallel=$PAR  TP OK join=$JOIN  TP-OK=$DONE  TIMEOUT=$TIMEOUTS  worker_parking=$PARK  worker_unparking=$WPARK"
if [ "$DONE" -ge 1 ] && [ "$PAR" -ge 7 ] && [ "$JOIN" -ge 1 ] && [ "$TIMEOUTS" -eq 0 ] \
   && [ "$PARK" -ge 1 ] && [ "$WPARK" -ge 1 ]; then
  echo "m2_tp: ThreadPool PASS — 7x parallel + 1x join, workers PARK and wake under the game's own Mono"
else
  echo "m2_tp: ThreadPool FAIL — a work item never ran (see $LOG)"
  exit 1
fi

echo "=== m2_tp: 2/2 timed-wait clock contract (host rows must match) ==="
bash tools/gate/t3_linux_probe.sh clock 2>&1 | tail -22 | sed 's/^/  /'
# t3_linux_probe.sh writes the TIER's stdout to obj/linux_probe_clock.log and the
# guest's console to the T3_LOG it prints there; the probe rows live in the console.
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$ROOT/obj/linux_probe_clock.log" | tail -1)"
if [ -n "$CONSOLE" ] && grep -aq '^\[LP\] done rc=0' "$CONSOLE"; then
  echo "m2_tp: clock PASS — gettimeofday is CLOCK_REALTIME and sem_timedwait honours an ms deadline"
else
  echo "m2_tp: clock FAIL — see obj/linux_probe_clock.log"
  exit 1
fi
echo "m2_tp: PASS"
exit 0
