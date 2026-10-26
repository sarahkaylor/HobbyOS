#!/usr/bin/env bash
# measure_build.sh — GB-006: REAL-build cold-vs-cached measurement (the DoD).
#
# cache_test.sh already proves ccache works on a hand-made probe.  The RED (from
# GB-006-V) was that the tree itself never goes through the cache (0 hits on the
# real build).  This script wraps the REAL Makefile C/C++ targets through the
# opt-in `/usr/lib/ccache` wiring (HOBBYOS_USE_CCACHE=1) and re-runs the gate's
# own shape:  COLD -> WARM -> NEGATIVE(clear) -- each phase `make clean`s first
# so the WARM recompile actually comes FROM the cache (and the NEGATIVE clears
# it and proves the delta is the cache, not page cache).  Raw counters at each.
set -euo pipefail
CCACHE="${CCACHE:-ccache}"
TARGET="${TARGET:-cxxrt_test_host errno_test_host rtti_test_host libc_string_test_host libc_ctype_test_host libc_printf_test_host libc_stdlib_test_host thrd_test_host tls_test_host torture_test_host}"
export HOBBYOS_USE_CCACHE=1        # activates the Makefile /usr/lib/ccache wiring
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/hobbyos-ccache}"; mkdir -p "$CCACHE_DIR"
export CCACHE_DIR
CCACHE_BASEDIR="${CCACHE_BASEDIR:-$PWD}"; export CCACHE_BASEDIR

secs(){ date +%s; }
phase(){ # arg: label; does  make clean + make, prints elapsed seconds
  make clean >/dev/null 2>&1 || true
  local t0 t1; t0=$(secs); make -s $TARGET >/dev/null 2>&1; t1=$(secs)
  echo "  $1 (make clean + make '${TARGET}'): $((t1-t0))s"
}
stats(){ "$CCACHE" -s 2>/dev/null | grep -iE "^  Compile requests|^  Hits:|^  Misses:" | awk -F: '{gsub(/^ +| +$/,"",$1); print "    " $1 ":" $2}'; }

echo "GB-006 REAL-build measurement  (HOBBYOS_USE_CCACHE=1  CCACHE_DIR=$CCACHE_DIR)"
echo "  TARGET='$TARGET'"

"$CCACHE" -C >/dev/null 2>&1 || true; "$CCACHE" -z >/dev/null 2>&1 || true
phase "COLD"
echo "  [ccache after COLD]"; stats
phase "WARM"
echo "  [ccache after WARM]"; stats
WARMHITS=$("$CCACHE" -s 2>/dev/null | grep -iE "^  [ ]*Hits:" | awk '{s+=$2} END{print int(s)}')
"$CCACHE" -C >/dev/null 2>&1 || true; "$CCACHE" -z >/dev/null 2>&1 || true
phase "NEGATIVE (cache cleared)"
echo "  [ccache after NEGATIVE]"; stats

echo "----"
[ "${WARMHITS:-0}" -ge 1 ] \
  && echo "PASS: real build (not a probe) served >=1 ccache hit (warm hits=${WARMHITS:-0})" \
  || { echo "FAIL: real build served 0 hits -- cache not wired into the tree"; exit 1; }
echo "GB-006 real-build measurement done."
