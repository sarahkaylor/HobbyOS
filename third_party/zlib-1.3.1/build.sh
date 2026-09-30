#!/bin/sh
# Host build recipe — zlib 1.3.1 (L6 lane browser/l6-libs1; see README.md).
#
# Extracts src/ from the pinned tarball if needed, builds a static lib with
# zlib's canonical configure, installs into build-host/prefix/, then runs the
# deflate/inflate roundtrip smoke test.  The WPE/port cross build reuses the
# same script: override CC / CFLAGS_EXTRA (and JOBS) — keep --static -fPIC.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
JOBS=${JOBS:-4}
CC=${CC:-cc}
CFLAGS_EXTRA=${CFLAGS_EXTRA:-}

echo "== zlib-1.3.1 build.sh: CC=$CC JOBS=$JOBS =="

[ -d "$SRC" ] || {
  mkdir -p "$SRC"
  tar -xzf "$HERE/zlib-1.3.1.tar.gz" -C "$SRC" --strip-components=1
}
mkdir -p "$PREFIX"

# zlib's configure has no out-of-tree mode; it builds in-tree under src/
# (gitignored scratch).  -fPIC keeps libz.a usable in shared-object links.
cd "$SRC"
CFLAGS="-O2 -fPIC $CFLAGS_EXTRA" ./configure --static --prefix="$PREFIX"
make -j"$JOBS"
make install

echo "-- installed --"
ls -l "$PREFIX/lib"

# --- smoke test: deflate/inflate roundtrip of a deterministic 64 KiB buffer
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/zlib_smoke.c" "$PREFIX/lib/libz.a" -o "$SMOKE_DIR/zlib_smoke"
"$SMOKE_DIR/zlib_smoke" > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
