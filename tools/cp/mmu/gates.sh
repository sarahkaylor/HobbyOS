#!/bin/bash
# tools/cp/mmu/gates.sh probes|unit|ram <args>  -- lane C1-mmu gate runner (linuxci).
#
#   probes [name...]   build the MODE=test image once, then run each Linux-ABI
#                      probe through tools/gate/t3_linux_probe.sh, one boot each,
#                      every boot under flock /tmp/cp-qemu.lock (shared box).
#                      Default list: every src/user/linux_probe_*.c except the
#                      X-session / fixture probes (x11 xwire wad glibc ext2).
#                      Prints one "GATE <name> PASS|FAIL" row per probe and the
#                      probe's [LP] rows + tail into obj/cpmmu/<name>.out.
#                      GATES_LOCKED=1: run as `flock LOCK gates.sh probes ...`
#                      (one lock hold for the batch instead of one per boot).
#   wave [name...]     smoke: every probe in ONE boot (see below).
#   unit               ./run_unit_tests_intel.sh under the lock (TOOLS_SKIP=1),
#                      summary of FAIL rows.
# Exit status: number of failing gates.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
OUT="$ROOT/obj/cpmmu"
mkdir -p "$OUT"
LOCK=/tmp/cp-qemu.lock
export TOOLS_SKIP=1
# Every QEMU boot runs under the shared lock -- unless the caller already holds
# it for a whole batch (GATES_LOCKED=1, e.g. `flock LOCK env GATES_LOCKED=1 ...`).
lk() { if [ "${GATES_LOCKED:-0}" = 1 ]; then "$@"; else flock "$LOCK" "$@"; fi; }
cmd=${1:?usage: gates.sh probes|wave|unit [names]}; shift
fails=0
case "$cmd" in
probes)
  if [ $# -eq 0 ]; then
    set -- $(ls src/user/linux_probe_*.c | sed 's/.*linux_probe_//; s/\.c$//' |
             grep -vxE 'x11|xwire|wad|glibc|ext2')
  fi
  make ARCH=intel MODE=test -j10 hobbyos.elf disk.img > "$OUT/build.log" 2>&1 ||
    { echo "GATE build FAIL (see $OUT/build.log)"; tail -20 "$OUT/build.log"; exit 99; }
  for n in "$@"; do
    lk bash tools/gate/t3_linux_probe.sh "$n" > "$OUT/$n.out" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then echo "GATE $n PASS"; else echo "GATE $n FAIL rc=$rc"; fails=$((fails+1)); fi
    grep -a '^\[LP\] done' "$OUT/$n.out" | tail -1 | sed "s/^/    $n: /"
    cp -f "obj/linux_probe_$n.log" "$OUT/$n.tier.log" 2>/dev/null
    c=$(sed -n 's/^T3_LOG=//p' "$OUT/$n.tier.log" 2>/dev/null | tail -1)
    [ -n "$c" ] && [ -r "$c" ] && cp -f "$c" "$OUT/$n.console.log"
    if [ -r "$OUT/$n.console.log" ]; then
      t=$(grep -acE '\[IDMAP-TRIP\]|\[KMEM-BADVA\]' "$OUT/$n.console.log")
      [ "$t" != 0 ] && echo "    $n: TRIPWIRE rows=$t"
    fi
  done
  ;;
wave)
  # Smoke only (not a gate of record): every probe in ONE boot, all launched
  # together from WAVE.CFG (Linux execve of an ELF is not supported, so a
  # sequential runner cannot exist yet).  Graded per probe from the kernel's
  # "[KERNEL] Process N: Pnn.ELF exited" + "[EXITPC] pid=N ... code=" rows.
  if [ $# -eq 0 ]; then
    set -- $(ls src/user/linux_probe_*.c | sed 's/.*linux_probe_//; s/\.c$//' |
             grep -vxE 'x11|xwire|wad|glibc|ext2')
  fi
  make ARCH=intel MODE=test -j10 hobbyos.elf disk.img > "$OUT/build.log" 2>&1 ||
    { echo "GATE build FAIL (see $OUT/build.log)"; tail -20 "$OUT/build.log"; exit 99; }
  : > "$OUT/WAVE.CFG"; : > "$OUT/stage.sh"; : > "$OUT/names"
  i=0
  for n in "$@"; do
    i=$((i+1)); f=$(printf 'P%02d.ELF' $i)
    make ARCH=intel "obj/intel/probe_$n.elf" >> "$OUT/build.log" 2>&1 || { echo "GATE $n BUILD FAIL"; exit 97; }
    echo "$f" >> "$OUT/WAVE.CFG"; echo "$f $n" >> "$OUT/names"
    echo "mcopy -o -i $ROOT/disk.img $ROOT/obj/intel/probe_$n.elf ::$f" >> "$OUT/stage.sh"
  done
  echo "mcopy -o -i $ROOT/disk.img $OUT/WAVE.CFG ::WAVE.CFG" >> "$OUT/stage.sh"
  lk env T3_TIMEOUT="${WAVE_TIMEOUT:-300}" T3_STAGE="bash $OUT/stage.sh" T3_MIN_LINES=1 \
    T3_QEMU_ARGS="${WAVE_QEMU_ARGS:-}" bash tools/t3_tier.sh intel > "$OUT/wave.tier.log" 2>&1
  c=$(sed -n 's/^T3_LOG=//p' "$OUT/wave.tier.log" | tail -1)
  C="$OUT/wave.console.log"
  [ -n "$c" ] && [ -r "$c" ] && cp -f "$c" "$C"
  while read -r f n; do
    pid=$(grep -a "Process [0-9]*: $f exited" "$C" | head -1 | sed 's/.*Process \([0-9]*\):.*/\1/')
    code=$( [ -n "$pid" ] && grep -a "^\[EXITPC\] pid=$pid " "$C" | head -1 | tr -d '\r' | sed 's/.*code=//')
    if [ "$code" = "0x0" ]; then echo "WAVE $n PASS (pid=$pid)"; else echo "WAVE $n FAIL pid=${pid:-?} code=${code:-none}"; fails=$((fails+1)); fi
  done < "$OUT/names"
  echo "    wave: done-rows=$(grep -ac '^\[LP\] done rc=' "$C" 2>/dev/null) rc0=$(grep -ac '^\[LP\] done rc=0' "$C" 2>/dev/null) halted=$(grep -ac 'System halt' "$C" 2>/dev/null)"
  t=$(grep -acE '\[IDMAP-TRIP\]|\[KMEM-BADVA\]' "$C" 2>/dev/null)
  echo "    wave: TRIPWIRE rows=${t:-?}"
  grep -aE '\[IDMAP-TRIP\]|\[KMEM-BADVA\]|FATAL kernel-mode' "$C" 2>/dev/null | head -5
  ;;
unit)
  lk ./run_unit_tests_intel.sh > "$OUT/unit.out" 2>&1
  rc=$?
  echo "GATE unit rc=$rc"
  grep -aE 'UNIT TESTS (PASSED|FAILED)|Tests (passed|failed)' "$OUT/unit.out" | tail -3
  grep -aE '^\s*FAIL|\[FAIL\]|FAILED:' "$OUT/unit.out" | head -20
  t=$(grep -acE '\[IDMAP-TRIP\]|\[KMEM-BADVA\]' "$OUT/unit.out")
  [ "$t" != 0 ] && echo "    unit: TRIPWIRE rows=$t"
  [ $rc -ne 0 ] && fails=1
  ;;
*) echo "unknown gate $cmd"; exit 2 ;;
esac
exit $fails
