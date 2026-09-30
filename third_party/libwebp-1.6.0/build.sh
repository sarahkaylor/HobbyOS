#!/bin/sh
# Host build recipe — libwebp 1.6.0 (L6 lane browser/l6-libs1; see README.md).
#
# Extracts src/ from the pinned tarball if needed, builds static libwebp +
# libwebpdemux + libwebpmux (tools off), installs into build-host/prefix/,
# then runs the lossless roundtrip smoke test.
# Override CC / JOBS for other toolchains.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
JOBS=${JOBS:-4}
CC=${CC:-cc}

echo "== libwebp-1.6.0 build.sh: CC=$CC JOBS=$JOBS =="
command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not on PATH (expect 3.31.x; see README.md)" >&2
  exit 1
}

[ -d "$SRC" ] || {
  mkdir -p "$SRC"
  tar -xzf "$HERE/libwebp-1.6.0.tar.gz" -C "$SRC" --strip-components=1
}
mkdir -p "$PREFIX"

# Library-only static build: CLI tools off (cwebp/dwebp need image I/O extras,
# gif2webp needs giflib, vwebp needs GLUT — none are needed for the WPE port).
cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_COMPILER="$CC" \
  -DBUILD_SHARED_LIBS=OFF \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
  -DWEBP_BUILD_FUZZTEST=OFF
cmake --build "$BUILD" -j "$JOBS"
cmake --install "$BUILD"

echo "-- installed --"
ls -l "$PREFIX/lib"

# --- smoke test: lossless RGBA roundtrip (must be byte-exact)
# libwebp.a references the bundled sharpyuv helper lib, so link both.
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/webp_smoke.c" "$PREFIX/lib/libwebp.a" "$PREFIX/lib/libsharpyuv.a" \
  -lm -lpthread -o "$SMOKE_DIR/webp_smoke"
"$SMOKE_DIR/webp_smoke" > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
