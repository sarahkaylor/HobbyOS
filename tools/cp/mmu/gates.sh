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
cmd=${1:?usage: gates.sh probes|unit [names]}; shift
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
    flock "$LOCK" bash tools/gate/t3_linux_probe.sh "$n" > "$OUT/$n.out" 2>&1
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
seq)
  # Smoke only: every probe in ONE boot, run one after another by runall.c.
  if [ $# -eq 0 ]; then
    set -- $(ls src/user/linux_probe_*.c | sed 's/.*linux_probe_//; s/\.c$//' |
             grep -vxE 'x11|xwire|wad|glibc|ext2')
  fi
  make ARCH=intel MODE=test -j10 hobbyos.elf disk.img > "$OUT/build.log" 2>&1 ||
    { echo "GATE build FAIL (see $OUT/build.log)"; tail -20 "$OUT/build.log"; exit 99; }
  musl-gcc -static-pie -fPIE -O1 -o "$OUT/runall.elf" tools/cp/mmu/runall.c || exit 98
  : > "$OUT/RUNALL.TXT"; : > "$OUT/stage.sh"
  i=0
  for n in "$@"; do
    i=$((i+1)); f=$(printf 'P%02d.ELF' $i)
    make ARCH=intel "obj/intel/probe_$n.elf" >> "$OUT/build.log" 2>&1 || { echo "GATE $n BUILD FAIL"; exit 97; }
    echo "$n /$f" >> "$OUT/RUNALL.TXT"
    echo "mcopy -o -i $ROOT/disk.img $ROOT/obj/intel/probe_$n.elf ::$f" >> "$OUT/stage.sh"
  done
  printf 'RUNALL.ELF\n' > "$OUT/WAVE.CFG"
  { echo "mcopy -o -i $ROOT/disk.img $OUT/runall.elf ::RUNALL.ELF"
    echo "mcopy -o -i $ROOT/disk.img $OUT/RUNALL.TXT ::RUNALL.TXT"
    echo "mcopy -o -i $ROOT/disk.img $OUT/WAVE.CFG ::WAVE.CFG"; } >> "$OUT/stage.sh"
  flock "$LOCK" env T3_TIMEOUT="${SEQ_TIMEOUT:-1500}" T3_STAGE="bash $OUT/stage.sh" T3_MIN_LINES=1 \
    T3_QEMU_ARGS="${SEQ_QEMU_ARGS:-}" bash tools/t3_tier.sh intel > "$OUT/seq.tier.log" 2>&1
  c=$(sed -n 's/^T3_LOG=//p' "$OUT/seq.tier.log" | tail -1)
  [ -n "$c" ] && [ -r "$c" ] && cp -f "$c" "$OUT/seq.console.log"
  grep -a '^\[LPRUN\]\|^\[LP\] done' "$OUT/seq.console.log" 2>/dev/null
  t=$(grep -acE '\[IDMAP-TRIP\]|\[KMEM-BADVA\]' "$OUT/seq.console.log" 2>/dev/null)
  echo "    seq: TRIPWIRE rows=${t:-?}"
  grep -aq '^\[LPRUN\] done ran=.* nonzero=0' "$OUT/seq.console.log" 2>/dev/null || fails=1
  ;;
unit)
  flock "$LOCK" ./run_unit_tests_intel.sh > "$OUT/unit.out" 2>&1
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
