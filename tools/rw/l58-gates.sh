#!/usr/bin/env bash
# tools/rw/l58-gates.sh — L58 regression set, sequential (never two boots at once).
#
# Order matters: the unit tier rebuilds hobbyos.elf/disk.img in place, so every
# desktop/probe gate after it starts from a clean full build (rule 5).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
export HOME=/home/sarah
OUT="$ROOT/obj/rw-desktop/l58-gates"
mkdir -p "$OUT"
pass=0; fail=0

run() { # name, command
  local name="$1"; shift
  echo "== $name"
  if "$@" >"$OUT/$name.log" 2>&1; then
    echo "   PASS"; pass=$((pass+1))
  else
    echo "   FAIL (see $OUT/$name.log)"; fail=$((fail+1))
  fi
}

run unit        env TOOLS_SKIP=1 ./run_unit_tests_intel.sh
run redzone     bash tools/gate/t3_linux_probe.sh redzone
run mmap_race   bash tools/gate/t3_linux_probe.sh mmap_race
run m2_mono     bash tools/gate/m2_mono.sh
run m2_tp       bash tools/gate/m2_tp.sh
run m2_threads  bash tools/gate/m2_threads.sh
run m12         bash tools/gate/m12_thread_ceiling.sh
run m3_window   python3 tools/gate/m3_window_probe.py
run m4_gl       python3 tools/gate/m4_gl_probe.py
run rwnfs       bash tools/rw/run-rwnfs-probe.sh

echo "== gates: $pass passed, $fail failed"
exit $((fail > 0))
