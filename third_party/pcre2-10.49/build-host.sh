#!/bin/sh
# build-host.sh -- host static build + smoke of the pinned pcre2 10.49
#                 (L6 lane browser/l6-glib; see README.md).
#
# Static-only 8-bit library (libpcre2-8.a) -- exactly what GLib 2.88.3 and the
# WPE 2.54.0 port consume via libpcre2-8.pc.  JIT is enabled (the 10.49
# security fix is in the JIT-stack path; the smoke proves it).  No 16/32-bit
# builds, no dependency-tracking.
#
# Extracts src/ from the pinned tarball via fetch.sh if needed, builds
# out-of-tree, installs into build-host/prefix/, then runs smoke/pcre2_smoke.c
# (static link, deterministic PASS/FAIL lines).
#
# Env knobs: JOBS (default 4), CC (default cc).
# Cross reuse: set CC + CFLAGS/--host as needed; keep --disable-shared
# --enable-static (see cross-notes.md).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
JOBS=${JOBS:-4}
CC=${CC:-cc}

log() { printf '[pcre2] %s\n' "$*"; }

log "build-host.sh: CC=$CC JOBS=$JOBS prefix=$PREFIX"

[ -d "$SRC" ] || sh "$HERE/fetch.sh"
mkdir -p "$BUILD" "$PREFIX"

cd "$BUILD"
if [ ! -f Makefile ]; then
  CC="$CC" "$SRC/configure" \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --enable-jit \
    --disable-dependency-tracking \
    CFLAGS="-O2 -fPIC"
fi
make -j"$JOBS"
make install

log "-- installed libs --"
ls -l "$PREFIX/lib"

# --- smoke test: static link against libpcre2-8.a, deterministic checks -----
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/pcre2_smoke.c" "$PREFIX/lib/libpcre2-8.a" \
  -o "$SMOKE_DIR/pcre2_smoke"
"$SMOKE_DIR/pcre2_smoke" > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
log "smoke result -> $HERE/build-host/SMOKE.RESULT"
