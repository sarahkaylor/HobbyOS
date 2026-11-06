#!/usr/bin/env bash
# M6/L63 final gates: the unit tier (rebuilds kernel+disk) and the four
# probes the page-in / NFS changes can break.  Never run this while the
# desktop soak is running (one guest at a time on this worktree).
set -uo pipefail
export HOME=/home/sarah
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
OUT="$ROOT/obj/rw-desktop/l63-final"
mkdir -p "$OUT"
pass=0; fail=0
run() {
  local name="$1"; shift
  echo "== $name"
  if "$@" >"$OUT/$name.log" 2>&1; then echo "   PASS"; pass=$((pass+1));
  else echo "   FAIL (see $OUT/$name.log)"; fail=$((fail+1)); fi
}
run unit        env TOOLS_SKIP=1 ./run_unit_tests_intel.sh
run mmap_race   bash tools/gate/t3_linux_probe.sh mmap_race
run m3_window   python3 tools/gate/m3_window_probe.py
run m4_gl       python3 tools/gate/m4_gl_probe.py
run rwnfs       bash tools/rw/run-rwnfs-probe.sh
echo "== gates: $pass passed, $fail failed"
exit $((fail > 0))
