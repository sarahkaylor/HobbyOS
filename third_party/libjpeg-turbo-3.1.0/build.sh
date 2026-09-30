#!/bin/sh
# Host build recipe — libjpeg-turbo 3.1.0 (L6 lane browser/l6-libs1; see README.md).
#
# Extracts src/ from the pinned tarball if needed, builds static libjpeg +
# libturbojpeg (SIMD on — NASM required), installs into build-host/prefix/,
# then runs the gradient encode/decode smoke test.
# Override CC / JOBS for other toolchains.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
JOBS=${JOBS:-4}
CC=${CC:-cc}

echo "== libjpeg-turbo-3.1.0 build.sh: CC=$CC JOBS=$JOBS =="
command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not on PATH (expect 3.31.x; see README.md)" >&2
  exit 1
}

[ -d "$SRC" ] || {
  mkdir -p "$SRC"
  tar -xzf "$HERE/libjpeg-turbo-3.1.0.tar.gz" -C "$SRC" --strip-components=1
}
mkdir -p "$PREFIX"

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_COMPILER="$CC" \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
  -DWITH_SIMD=ON -DWITH_TURBOJPEG=ON \
  -DWITH_12BIT=OFF -DWITH_FUZZ=OFF -DWITH_JAVA=OFF
cmake --build "$BUILD" -j "$JOBS"
cmake --install "$BUILD"

echo "-- build config (CMakeCache.txt excerpt) --"
grep -E '^(ENABLE_SHARED|ENABLE_STATIC|WITH_SIMD|WITH_TURBOJPEG|WITH_JPEG8|NASM_EXECUTABLE):' "$BUILD/CMakeCache.txt" || true
echo "-- installed --"
ls -l "$PREFIX/lib"

# --- smoke test: encode a gradient (q95) + decode + compare
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/jpeg_smoke.c" "$PREFIX/lib/libjpeg.a" -lm -o "$SMOKE_DIR/jpeg_smoke"
"$SMOKE_DIR/jpeg_smoke" > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
