#!/bin/sh
# L6 lane (browser/l6-libs1) host gate: builds + smoke-tests the four
# image/foundation libs in dependency order (zlib first — libpng links it).
#
# Usage:
#   third_party/l6-libs1-gate.sh           # incremental: only what is missing
#   third_party/l6-libs1-gate.sh --clean   # wipe src/ + build-host/ first:
#                                          # full from-tarball rebuild
#
# Checksum-stability evidence: run once, run again with --clean, then diff the
# SMOKE.RESULT lines and the installed static-library sha256 list below.
set -eu
TP=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBS="zlib-1.3.1 libpng-1.6.44 libjpeg-turbo-3.1.0 libwebp-1.6.0"
CLEAN=0
if [ "${1:-}" = "--clean" ]; then CLEAN=1; fi

for lib in $LIBS; do
  if [ "$CLEAN" = 1 ]; then
    rm -rf "$TP/$lib/src" "$TP/$lib/build-host"
  fi
  echo "==== gate: build + smoke: $lib ===="
  "$TP/$lib/build.sh"
done

echo "==== gate: smoke results ===="
for lib in $LIBS; do
  cat "$TP/$lib/build-host/SMOKE.RESULT"
  grep -q ': OK' "$TP/$lib/build-host/SMOKE.RESULT" || {
    echo "FATAL: $lib smoke did not report OK" >&2
    exit 1
  }
done

echo "==== gate: installed static-library sha256 ===="
sha256sum "$TP"/*/build-host/prefix/lib/*.a | sort

echo "==== gate: PASS — all four host builds + smokes green ===="
