#!/usr/bin/env bash
# M6/L61 final gates: the full tier, sequential (never two boots at once).
# Same list as L60's gates so a regression is a delta, not a new baseline.
set -uo pipefail
export HOME=/home/sarah
ROOT=/home/sarah/hobbyos-rw-lanes/l61-jit
cd "$ROOT" || exit 2
OUT="$ROOT/obj/l61/gates"
mkdir -p "$OUT"
pass=0; fail=0
run() {
  local name="$1"; shift
  echo "== $name"
  if "$@" >"$OUT/$name.log" 2>&1; then echo "   PASS $name"; pass=$((pass+1));
  else echo "   FAIL $name (see $OUT/$name.log)"; fail=$((fail+1)); fi
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
