#!/usr/bin/env bash
# build-host.sh -- one-shot host build + smoke of the vendored ICU4C 78.3
#                 (third_party/icu-78.3; L6 lane browser/l6-icu).
#
#   ./build-host.sh                  # JOBS=4; override JOBS / CC / CXX /
#                                    # ICU_PREFIX / CONFIGURE_EXTRA
#
# Stages: fetch.sh (download + 3-digest gate + extract) -> out-of-tree
# configure (static libs, --with-data-packaging=static) -> make -> make
# install -> smoke/run-smoke.sh.  All build output is teed to build.log;
# smoke runs land in build-host/smoke/ + build-host/SMOKE.RESULT.
#
# Why these flags (full rationale in README.md):
#   --disable-shared --enable-static   static libs for a bare-metal target
#   --with-data-packaging=static       data compiled into libicudata.a; no
#                                      .dat file / filesystem at run time
#   --disable-samples/tests/extras     nothing WebKit needs
#   --disable-icuio                    libicuio (C++ streams) unused
#   tools stay ENABLED                 genrb/pkgdata/icupkg are needed for the
#                                      data build, and the same build tree is
#                                      the --with-cross-build host-tools
#                                      staging area for the future target
#                                      cross build (see cross-notes.md)
#   renaming stays ENABLED (default)   WPE 2.54 defines U_DISABLE_RENAMING=1
#                                      only on Apple; Linux/WPE builds use the
#                                      default versioned symbols, so the
#                                      library must be built the same way
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-4}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
SRC="$VENDOR_DIR/src"
BUILD_ROOT="$VENDOR_DIR/build-host"
BUILD_DIR="$BUILD_ROOT/obj"
PREFIX="${ICU_PREFIX:-$BUILD_ROOT/prefix}"
CONFIGURE_EXTRA="${CONFIGURE_EXTRA:-}"

mkdir -p "$BUILD_ROOT"
exec > >(tee -a "$BUILD_ROOT/build.log") 2>&1

echo "== icu-78.3 build-host.sh: JOBS=$JOBS CC=$CC CXX=$CXX =="
echo "== host build start: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="

"$VENDOR_DIR/fetch.sh"

if [ ! -f "$SRC/source/data/in/icudt78l.dat" ] && [ -f "$SRC/source/data/locales/root.txt" ]; then
  echo "== NOTE: prebuilt data/in/icudt78l.dat is absent while data sources are"
  echo "==       present -> this build will rebuild the data from source"
  echo "==       (no ICU_DATA_FILTER_FILE -> unfiltered; slower)."
  echo "==       build-host-trimmed.sh parked the prebuilt archive."
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/config.status" ]; then
  "$SRC/source/configure" \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --disable-samples --disable-tests --disable-extras --disable-icuio \
    --with-data-packaging=static \
    CC="$CC" CXX="$CXX" $CONFIGURE_EXTRA
else
  echo "== configure already done in $BUILD_DIR (rm -rf it for a fresh build) =="
fi

make -j"$JOBS"
make install

echo "-- installed static libs --"
ls -la "$PREFIX"/lib/libicu*.a
echo "-- sizes (prefix/lib) --"
du -sh "$PREFIX/lib"
for f in "$PREFIX"/lib/libicu*.a; do
  printf '%-56s %10s bytes\n' "${f#"$PREFIX"/}" "$(stat -c%s "$f")"
done

"$VENDOR_DIR/smoke/run-smoke.sh"

echo "== host build done: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="
