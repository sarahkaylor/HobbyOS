#!/usr/bin/env bash
# build-host.sh -- host static build + smoke of the pinned libffi 3.4.8
#                 (L6 lane browser/l6-glib; see README.md).
#
# Static-only (libffi.a), docs disabled (no texinfo needed).  This is what
# GLib's GObject closures link against (g_cclosure_marshal_generic is
# libffi-backed; glib-2.88.3/meson.build:2301 dependency('libffi', >=3.0.0)).
#
# Extracts src/ from the pinned tarball via fetch.sh if needed, builds
# out-of-tree with autotools, installs into build-host/prefix/, then runs
# smoke/ffi_smoke.c (static link: ffi_call + ffi_closure trampoline).
#
# Env knobs: JOBS (default 4), CC (default cc).
# Cross reuse: set CC / --host / --target as needed; keep --disable-shared
# --enable-static (see cross-notes.md).
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
JOBS=${JOBS:-4}
CC=${CC:-cc}

log() { printf '[libffi] %s\n' "$*"; }

log "build-host.sh: CC=$CC JOBS=$JOBS prefix=$PREFIX"

[ -d "$SRC" ] || "$HERE/fetch.sh"
mkdir -p "$BUILD" "$PREFIX"

cd "$BUILD"
if [ ! -f Makefile ]; then
  CC="$CC" CFLAGS="-O2 -fPIC" "$SRC/configure" \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --disable-docs
fi
make -j"$JOBS"
make install

log "-- installed libs --"
ls -l "$PREFIX/lib"

# --- smoke test: static link against libffi.a, deterministic checks ---------
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
"$CC" -O2 -Wall -Wextra -I"$PREFIX/include" \
  "$HERE/smoke/ffi_smoke.c" "$PREFIX/lib/libffi.a" \
  -o "$SMOKE_DIR/ffi_smoke"
"$SMOKE_DIR/ffi_smoke" | tee "$HERE/build-host/SMOKE.RESULT"
log "smoke result -> $HERE/build-host/SMOKE.RESULT"
