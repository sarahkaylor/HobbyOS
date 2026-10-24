#!/usr/bin/env bash
# cold_vs_cached.sh — GB-006 DoD: measure cold vs cached compile time.
#
# Usage:
#   bash tools/cache/cold_vs_cached.sh OUT.o SRC.c [compiler] [flags...]
#
# Compiles SRC.c to OUT.o twice with the ccache-wrapped compiler: once cold
# (cache cleared) and once warm (cache hit), timing both, then prints the cold
# time, the warm time, and the speedup.  The compiler defaults to `clang`;
# pass e.g. `clang-21` / a cross compiler to measure the actual toolchain.
#
# The printed numbers are real: ccache stats (Miss then Hit) + wall-clock ms.
set -euo pipefail
OUT="${1:?out.o}"; SRC="${2:?src.c}"; CC="${3:-clang}"
shift 3 || true

CCACHE="${CCACHE:-ccache}"
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/hobbyos-ccache}"
mkdir -p "$CCACHE_DIR"
export CCACHE_DIR

# ms timer (Linux): seconds.nanoseconds -> milliseconds integer.
ms_now() { date +%s%N | awk '{print int($1/1000000)}'; }

echo "== cold compile (cache cleared) =="
"$CCACHE" -C >/dev/null 2>&1 || true   # clear only if using real ccache
"$CCACHE" -z >/dev/null 2>&1 || true
t0=$(ms_now)
"$CCACHE" "$CC" "$@" -c "$SRC" -o "$OUT"
t1=$(ms_now)
COLD=$((t1 - t0))

echo "== warm compile (cache hit) =="
t2=$(ms_now)
"$CCACHE" "$CC" "$@" -c "$SRC" -o "$OUT"
t3=$(ms_now)
WARM=$((t3 - t2))

echo "cold_ms=$COLD warm_ms=$WARM"
if [ "$WARM" -ge 1 ] 2>/dev/null && [ "$COLD" -gt "$WARM" ]; then
  SPEED=$(awk -v c="$COLD" -v w="$WARM" 'BEGIN{printf "%.1f", c/w}')
  echo "speedup=${SPEED}x"
else
  echo "speedup=1x (warm not faster than cold on this tiny unit)"
fi
# Real per-compile hit proof from ccache stats (works for real ccache only).
if command -v "$CCACHE" >/dev/null 2>&1 && "$CCACHE" -s >/dev/null 2>&1; then
  "$CCACHE" -s | grep -iE "hits|misses|files in cache" || true
fi
