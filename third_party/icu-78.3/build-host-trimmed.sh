#!/usr/bin/env bash
# build-host-trimmed.sh -- ICU 78.3 host build with a *source-built, trimmed*
#                          data set (third_party/icu-78.3; lane browser/l6-icu).
#
# Resolves the browser.md F0 open item "data-trimming approach still open —
# resolve at the L6 ICU build" with a working mechanism and measured numbers:
#
#   full build (prebuilt data, build-host.sh):   libicudata.a  33,108,268 B
#   this build (en locales, no brkitr dicts):    libicudata.a  10,196,012 B
#   (libicuuc.a / libicui18n.a are unchanged — they are code, not data)
#
# How it works / why each step exists:
#   1. fetch.sh --data-src   downloads the official icu4c-78.3-data.zip (the
#                            data *sources*; NOT in the sources tarball) and
#                            overlays it into src/source/data/.
#   2. park the prebuilt data/in/icudt78l.dat.  ICU's makefile build uses
#      the prebuilt archive whenever it exists -- and then
#      ICU_DATA_FILTER_FILE is silently ignored (verified: identical output
#      sizes).  With the archive absent, data/rules.mk (generated at
#      configure time, with the filter) drives a real genrb/genbrk build.
#   3. configure with ICU_DATA_FILTER_FILE=data-filter-en.json
#      (see that file + README.md "Data packaging strategy").
#   4. build, install to build-host/prefix-trim-src/, print sizes, and run
#      the smoke against the trimmed prefix (tagged so it does not clobber
#      the canonical full-build SMOKE.RESULT).
#
# NOTE: parking the prebuilt archive flips *every later build in this src/
# tree* to the build-from-source data path (slower).  To restore the plain
# release behaviour, delete src/ and re-run fetch.sh (no --data-src).
#
# Env knobs: JOBS (default 4), CC, CXX, ICU_PREFIX.
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-4}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
SRC="$VENDOR_DIR/src"
BUILD_ROOT="$VENDOR_DIR/build-host"
BUILD_DIR="$BUILD_ROOT/obj-trim-src"
PREFIX="${ICU_PREFIX:-$BUILD_ROOT/prefix-trim-src}"
FILTER="$VENDOR_DIR/data-filter-en.json"

mkdir -p "$BUILD_ROOT"
exec > >(tee -a "$BUILD_ROOT/trimmed.log") 2>&1

echo "== icu-78.3 build-host-trimmed.sh: JOBS=$JOBS CC=$CC CXX=$CXX =="
echo "== trimmed host build start: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="

bash "$VENDOR_DIR/fetch.sh" --data-src

if [ -f "$SRC/source/data/in/icudt78l.dat" ]; then
  echo "== parking prebuilt data/in/icudt78l.dat -> icudt78l.dat.parked-prebuilt"
  echo "==   (present prebuilt archive makes ICU ignore ICU_DATA_FILTER_FILE)"
  mv "$SRC/source/data/in/icudt78l.dat" "$SRC/source/data/in/icudt78l.dat.parked-prebuilt"
else
  echo "== prebuilt data/in/icudt78l.dat already parked (or absent)"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/config.status" ]; then
  ICU_DATA_FILTER_FILE="$FILTER" \
  "$SRC/source/configure" \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --disable-samples --disable-tests --disable-extras --disable-icuio \
    --with-data-packaging=static \
    CC="$CC" CXX="$CXX"
else
  echo "== configure already done in $BUILD_DIR (rm -rf it for a fresh build) =="
fi

make -j"$JOBS"
make install

echo "-- trimmed static libs --"
ls -la "$PREFIX"/lib/libicu*.a
echo "-- sizes (prefix/lib) --"
du -sh "$PREFIX/lib"
for f in "$PREFIX"/lib/libicu*.a; do
  printf '%-56s %10s bytes\n' "${f#"$PREFIX"/}" "$(stat -c%s "$f")"
done

SMOKE_TAG=trim-src ICU_PREFIX="$PREFIX" bash "$VENDOR_DIR/smoke/run-smoke.sh"

echo "== trimmed host build done: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="
