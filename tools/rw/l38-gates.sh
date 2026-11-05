#!/usr/bin/env bash
# L38 gate set (run after the last desktop build; each gate rebuilds the image,
# so they are strictly sequential and the harness must be re-run afterwards --
# skill rules 5/55).
set -uo pipefail
ROOT=/home/sarah/hobbyos-rw-lanes/l38-jit
cd "$ROOT" || exit 2
export HOME=/home/sarah
EV="$ROOT/obj/rw-desktop/evidence-l38"
mkdir -p "$EV"
run() {
  local tag="$1"; shift
  echo "=== $tag $(date +%H:%M:%S)"
  TOOLS_SKIP=1 "$@" > "$EV/gate-$tag.log" 2>&1
  local rc=$?
  echo "--- $tag rc=$rc"
  grep -aE 'PASS|FAIL|Tests run|Tests failed|\[LP\] done|PRESENTED|redzone' "$EV/gate-$tag.log" | tail -6
  echo "rc=$rc" > "$EV/gate-$tag.rc"
}
run redzone  bash tools/gate/t3_linux_probe.sh redzone
run m2mono   bash tools/gate/m2_mono.sh
run m2threads bash tools/gate/m2_threads.sh
run m12      bash tools/gate/m12_thread_ceiling.sh
run m3window  python3 tools/gate/m3_window_probe.py
run m4gl     python3 tools/gate/m4_gl_probe.py
echo "=== gates done $(date +%H:%M:%S)"
for f in "$EV"/gate-*.rc; do printf "%s: %s\n" "$(basename "$f")" "$(cat "$f")"; done
