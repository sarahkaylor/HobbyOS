#!/usr/bin/env bash
# M6/L63 final gates: the unit tier (which rebuilds kernel+disk), the FPU/signal
# probe, then the desktop probes that exercise the image path this lane changed.
# One tier at a time: the unit wave and a desktop boot must never share the
# machine (they both own disk.img and the console).
set -uo pipefail
export HOME=/home/sarah
ROOT=/home/sarah/hobbyos-rw-lanes/l67-wedge
cd "$ROOT" || exit 2
OUT="$ROOT/obj/rw-desktop/l67-final"
mkdir -p "$OUT"
pass=0; fail=0
run() {
  local name="$1"; shift
  echo "== $name"
  if "$@" >"$OUT/$name.log" 2>&1; then echo "   PASS"; pass=$((pass+1));
  else echo "   FAIL (see $OUT/$name.log)"; fail=$((fail+1)); fi
}
run redzone     bash tools/gate/t3_linux_probe.sh redzone
run fpu_sig     env LP_WAVE_EXTRA="PROBE.ELF nest" bash tools/gate/t3_linux_probe.sh fpu_sig
run m2_mono     bash tools/gate/m2_mono.sh
run m2_tp       bash tools/gate/m2_tp.sh
run m2_threads  bash tools/gate/m2_threads.sh
run m12         bash tools/gate/m12_thread_ceiling.sh

run mmap_race   bash tools/gate/t3_linux_probe.sh mmap_race
run m3_window   python3 tools/gate/m3_window_probe.py
run m4_gl       python3 tools/gate/m4_gl_probe.py
run rwnfs       bash tools/rw/run-rwnfs-probe.sh
echo "== gates: $pass passed, $fail failed"
exit $((fail > 0))
