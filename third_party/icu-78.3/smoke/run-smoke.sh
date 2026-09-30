#!/usr/bin/env bash
# run-smoke.sh — compile + run the ICU 78.3 host smoke (third_party/icu-78.3,
# lane browser/l6-icu; see README.md, "Smoke test").
#
# The smoke is run twice:
#   run 1: ICU_DATA unset
#   run 2: ICU_DATA=/nonexistent-icu-data
# With --with-data-packaging=static the data is linked into libicudata.a, so
# both runs must be byte-identical and pass — that is the data-strategy proof.
#
# Env knobs: ICU_PREFIX (default build-host/prefix), CC, CXX.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR_DIR="$(cd "$HERE/.." && pwd)"
PREFIX="${ICU_PREFIX:-$VENDOR_DIR/build-host/prefix}"
SMOKE_TAG="${SMOKE_TAG:-}"
OUT="$VENDOR_DIR/build-host/smoke${SMOKE_TAG:+-$SMOKE_TAG}"
RESULT="$VENDOR_DIR/build-host/SMOKE.RESULT${SMOKE_TAG:+.${SMOKE_TAG}}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"

[ -f "$PREFIX/lib/libicui18n.a" ] || {
  echo "error: static ICU not built at $PREFIX — run build-host.sh first" >&2
  exit 1
}

mkdir -p "$OUT"
echo "-- compiling smoke --"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" -c "$HERE/icu_smoke.c" -o "$OUT/icu_smoke.o"
"$CXX" "$OUT/icu_smoke.o" -o "$OUT/icu_smoke" \
  -L"$PREFIX/lib" -licui18n -licuuc -licudata -lpthread -ldl -lm

echo "-- run 1: ICU_DATA unset --"
env -u ICU_DATA "$OUT/icu_smoke" | tee "$OUT/smoke-run1.log"
echo "-- run 2: ICU_DATA=/nonexistent-icu-data --"
ICU_DATA=/nonexistent-icu-data "$OUT/icu_smoke" | tee "$OUT/smoke-run2.log"

if ! diff -u "$OUT/smoke-run1.log" "$OUT/smoke-run2.log" > "$OUT/smoke-diff.txt"; then
  echo "FAIL: smoke output differs with ICU_DATA set to a nonexistent path" >&2
  echo "      (static data packaging must be filesystem-independent)" >&2
  exit 1
fi
rm -f "$OUT/smoke-diff.txt"

grep -qx "ALL TESTS PASSED SUCCESSFULLY!" "$OUT/smoke-run1.log" || {
  echo "FAIL: smoke did not reach ALL TESTS PASSED SUCCESSFULLY!" >&2
  exit 1
}

cp "$OUT/smoke-run1.log" "$RESULT"
echo "-- SMOKE.RESULT (verdict tail) --"
tail -3 "$RESULT"
