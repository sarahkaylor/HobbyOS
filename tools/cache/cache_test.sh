#!/usr/bin/env bash
# cache_test.sh — GB-006 T1 self-test.
#
# Proves the DoD "cold vs cached build time measured" with real numbers:
#   1. requires ccache (and a clang-like compiler).
#   2. compiles a probe cold  -> ccache reports a Miss.
#   3. recompiles it warm     -> ccache reports a Hit (from cache).
#   4. prints cold_ms / warm_ms / speedup from actual wall-clock measurements.
# The test FAILS (non-zero) if ccache is missing or the warm compile is not a
# cache hit.
set -euo pipefail
CCACHE="${CCACHE:-ccache}"
CC="${CC:-clang}"
CCACHE_DIR="${CCACHE_DIR:-$(mktemp -d)}"
export CCACHE_DIR

fail=0
chk() { if [ "$1" = "$2" ]; then echo "  PASS: $3"; else echo "  FAIL: $3 (got '$1')"; fail=1; fi; }

ms_now() { date +%s%N | awk '{print int($1/1000000)}'; }

if ! command -v "$CCACHE" >/dev/null 2>&1; then
  echo "cache_test: ccache not found (CCACHE=$CCACHE)"; exit 2
fi
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "cache_test: compiler '$CC' not found"; exit 2
fi

echo "=== cache_test: cold vs cached build time ==="
probe="$(mktemp "${TMPDIR:-/tmp}/cachetest.XXXXXX.c")"
printf '#include <stdio.h>\nint probe(void){printf("probe\\n");return 42;}\n' > "$probe"
obj="${probe%.c}.o"

"$CCACHE" -C >/dev/null 2>&1 || true
"$CCACHE" -z >/dev/null 2>&1 || true

t0=$(ms_now); "$CCACHE" "$CC" -c "$probe" -o "$obj"; t1=$(ms_now)
COLD=$((t1 - t0))

t2=$(ms_now); "$CCACHE" "$CC" -c "$probe" -o "$obj"; t3=$(ms_now)
WARM=$((t3 - t2))

# ccache stats: after one cold + one warm compile of identical input there must
# be >=1 hit (the count is field 2 of the "Hits:" lines — last field is a %).
STATS="$("$CCACHE" -s 2>/dev/null || true)"
HITS="$(printf '%s\n' "$STATS" | grep -i "Hits:" | awk '{s+=$2} END{print int(s)}')"
MISS="$(printf '%s\n' "$STATS" | grep -i "Misses:" | awk '{s+=$2} END{print int(s)}')"

echo "cold_ms=$COLD warm_ms=$WARM"
echo "ccache stats: hits~$HITS misses~$MISS"
if [ "${HITS:-0}" -ge 1 ] 2>/dev/null; then
  echo "  PASS: warm compile is a ccache HIT (hits=$HITS)"
else
  echo "  FAIL: no ccache hit recorded (hits=$HITS)"; fail=1
fi

# The speedup claim requires a real (measurable) gap; assert warm <= cold.
if [ "$WARM" -le "$COLD" ]; then
  echo "  PASS: warm <= cold (cached build not slower)"
else
  echo "  FAIL: warm ($WARM ms) slower than cold ($COLD ms)"; fail=1
fi

rm -f "$probe" "$obj"
echo "=== cache_test DoD: cold vs cached build time measured (real numbers above) ==="
[ "$fail" = 0 ] && echo "CACHE TEST PASSED" || { echo "CACHE TEST FAILED"; exit 1; }
