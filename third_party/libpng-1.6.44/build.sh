#!/bin/sh
# Host build recipe — libpng 1.6.44 (L6 lane browser/l6-libs1; see README.md).
#
# Extracts src/ from the pinned tarball if needed, builds a static libpng
# linked against the third_party zlib build, installs into build-host/prefix/,
# then runs the generated-PNG decode/re-encode smoke test.
# Override ZLIB_PREFIX to link a different zlib; CC / JOBS likewise.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
ZLIB_PREFIX=${ZLIB_PREFIX:-"$HERE/../zlib-1.3.1/build-host/prefix"}
JOBS=${JOBS:-4}
CC=${CC:-cc}

echo "== libpng-1.6.44 build.sh: CC=$CC JOBS=$JOBS ZLIB_PREFIX=$ZLIB_PREFIX =="
command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not on PATH (expect 3.31.x; see README.md)" >&2
  exit 1
}
[ -f "$ZLIB_PREFIX/include/zlib.h" ] || {
  echo "error: zlib not built yet — run third_party/zlib-1.3.1/build.sh first (or set ZLIB_PREFIX)" >&2
  exit 1
}

[ -d "$SRC" ] || {
  mkdir -p "$SRC"
  tar -xJf "$HERE/libpng-1.6.44.tar.xz" -C "$SRC" --strip-components=1
}
mkdir -p "$PREFIX"

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_PREFIX_PATH="$ZLIB_PREFIX" \
  -DZLIB_ROOT="$ZLIB_PREFIX" \
  -DPNG_SHARED=OFF -DPNG_STATIC=ON \
  -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
cmake --build "$BUILD" -j "$JOBS"
cmake --install "$BUILD"

# The vendored zlib must be the one libpng links (not the system zlib).
ZLIB_CACHE=$(grep -E '^ZLIB_[A-Z_]+:[a-zA-Z]+=' "$BUILD/CMakeCache.txt" || true)
echo "-- zlib resolution --"
echo "$ZLIB_CACHE"
case "$ZLIB_CACHE" in
  *"$ZLIB_PREFIX"*) ;;
  *) echo "error: libpng did not resolve zlib to $ZLIB_PREFIX" >&2; exit 1 ;;
esac

echo "-- installed --"
ls -l "$PREFIX/lib"

# --- smoke test: write PNG -> decode -> re-encode -> decode, all byte-exact
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/png_smoke.c" "$PREFIX/lib/libpng.a" "$ZLIB_PREFIX/lib/libz.a" -lm \
  -o "$SMOKE_DIR/png_smoke"
"$SMOKE_DIR/png_smoke" "$SMOKE_DIR/png_smoke_1.png" "$SMOKE_DIR/png_smoke_2.png" \
  > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
