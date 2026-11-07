#!/usr/bin/env bash
# L37 regression set: the five gates the lane must keep green, one at a time
# (never concurrent with the desktop harness: they share disk.img).
set -uo pipefail
ROOT=/home/sarah/hobbyos-rw-lanes/l37-garbage
cd "$ROOT" || exit 2
export HOME=/home/sarah
export TOOLS_SKIP=1
EV="$ROOT/obj/rw-desktop/evidence-l37"
for g in "bash tools/gate/m2_mono.sh:m2_mono" \
         "bash tools/gate/m2_threads.sh:m2_threads" \
         "bash tools/gate/m12_thread_ceiling.sh:m12" \
         "python3 tools/gate/m3_window_probe.py:m3_window_probe" \
         "python3 tools/gate/m4_gl_probe.py:m4_gl_probe"; do
  cmd="${g%%:*}"; name="${g##*:}"
  echo "=== $name $(date +%H:%M:%S)"
  $cmd > "$EV/gate-$name.log" 2>&1
  rc=$?
  echo "=== $name rc=$rc"
  tail -3 "$EV/gate-$name.log" | sed 's/^/    /'
done
echo "=== gates done $(date +%H:%M:%S)"
