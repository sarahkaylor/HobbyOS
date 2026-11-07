#!/usr/bin/env bash
# tools/rw/l75-gates.sh — the L73 regression ladder, one log per gate.
#
# Every gate boots the guest, so they run strictly one at a time; the unit tier
# is separate (TOOLS_SKIP=1 ./run_unit_tests_intel.sh) and must never run
# concurrently with a desktop harness.
#
# Usage: tools/rw/l75-gates.sh [gate ...]      (default: all)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
OUT="$ROOT/obj/l75/gates"
mkdir -p "$OUT"

ALL="redzone mmap_race m2_mono m2_tp m2_threads m12 m3_window m4_gl rwnfs"
WANT="${*:-$ALL}"

run_one() {
  local name="$1"
  local log="$OUT/$name.log"
  case "$name" in
    redzone|mmap_race) bash tools/gate/t3_linux_probe.sh "$name" ;;
    m2_mono)           bash tools/gate/m2_mono.sh ;;
    m2_tp)             bash tools/gate/m2_tp.sh ;;
    m2_threads)        bash tools/gate/m2_threads.sh ;;
    m12)               bash tools/gate/m12_thread_ceiling.sh ;;
    m3_window)         python3 tools/gate/m3_window_probe.py ;;
    m4_gl)             python3 tools/gate/m4_gl_probe.py ;;
    rwnfs)             bash tools/rw/run-rwnfs-probe.sh ;;
    *) echo "unknown gate $name"; return 2 ;;
  esac >"$log" 2>&1
}

for g in $WANT; do
  echo "== gate $g"
  t0=$(date +%s)
  run_one "$g"
  rc=$?
  t1=$(date +%s)
  verdict=$(grep -aoE "PASS|FAIL|FAILED|OK|TIMEOUT|MISSING" "$OUT/$g.log" | tail -1)
  echo "   rc=$rc $((t1-t0))s verdict=${verdict:-none} log=obj/l75/gates/$g.log"
done
