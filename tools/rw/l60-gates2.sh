#!/usr/bin/env bash
# M6/L60: the gates that exercise the code this lane changed (image path,
# exec page-in, mmap).  The thread/mono/NFS probes were run on the same build
# family in tools/rw/l60-gates.sh; see docs §47.8.
set -uo pipefail
export HOME=/home/sarah
ROOT=/home/sarah/hobbyos-rw-lanes/l60-ud
cd "$ROOT" || exit 2
OUT="$ROOT/obj/rw-desktop/l60-final2"
mkdir -p "$OUT"
pass=0; fail=0
run() {
  local name="$1"; shift
  echo "== $name"
  if "$@" >"$OUT/$name.log" 2>&1; then echo "   PASS $name"; pass=$((pass+1));
  else echo "   FAIL $name (see $OUT/$name.log)"; fail=$((fail+1)); fi
}
run unit        env TOOLS_SKIP=1 ./run_unit_tests_intel.sh
run m3_window   python3 tools/gate/m3_window_probe.py
run m4_gl       python3 tools/gate/m4_gl_probe.py
run rwnfs       bash tools/rw/run-rwnfs-probe.sh
echo "== gates: $pass passed, $fail failed"
exit $((fail > 0))
