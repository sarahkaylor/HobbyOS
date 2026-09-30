#!/bin/sh
# build-host.sh -- host static build + smoke of the pinned GLib 2.88.3
#                 (L6 lane browser/l6-glib; see README.md and cross-notes.md).
#
# SHARED orchestrator for the GLib host-first trio: builds the hard deps first
# (pcre2-10.49, libffi-3.4.8 via their own build-host.sh; zlib-1.3.1 reuses the
# lane l6-libs1 recipe), then configures GLib 2.88.3 against those prefixes
# (meson, static libs), installs into build-host/prefix/, and runs
# smoke/glib_smoke.c -- a static-link smoke covering GString / GHashTable /
# GPtrArray / g_ascii / GRegex (PCRE2-backed) / GDateTime / GMutex+GThread /
# GThreadPool plus GObject / GModule / GIO.
#
# Options chosen for a host-first, cross-reusable recipe:
#   -Ddefault_library=static   only .a archives (WPE links these statically)
#   -Dnls=disabled             no gettext (see README.md "gettext" note)
#   -Dselinux/-Dlibmount=disabled, -Dsysprof=disabled   no system lib deps
#   -Dman-pages/-Ddocumentation=false, -Dintrospection=disabled, -Dtests=false
#   --wrap-mode=nofallback     fail loudly instead of downloading wrap deps
#
# Env knobs: JOBS (default 8), CC, PCRE2_DIR, LIBFFI_DIR, ZLIB_PREFIX.
# Cross reuse: meson cross file + CC/toolchain (see cross-notes.md).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TP=$(CDPATH= cd -- "$HERE/.." && pwd)
SRC="$HERE/src"
PREFIX="$HERE/build-host/prefix"
BUILD="$HERE/build-host/build"
JOBS=${JOBS:-8}
CC=${CC:-cc}
PCRE2_DIR=${PCRE2_DIR:-"$TP/pcre2-10.49"}
LIBFFI_DIR=${LIBFFI_DIR:-"$TP/libffi-3.4.8"}
ZLIB_PREFIX=${ZLIB_PREFIX:-"$TP/zlib-1.3.1/build-host/prefix"}

log() { printf '[glib] %s\n' "$*"; }
die() { printf '[glib] ERROR: %s\n' "$*" >&2; exit 1; }

log "build-host.sh: CC=$CC JOBS=$JOBS prefix=$PREFIX"
log "deps: PCRE2_DIR=$PCRE2_DIR LIBFFI_DIR=$LIBFFI_DIR ZLIB_PREFIX=$ZLIB_PREFIX"

# --- 0. hard deps, built by their own recipes (idempotent) -------------------
[ -f "$PCRE2_DIR/build-host/prefix/lib/libpcre2-8.a" ] || sh "$PCRE2_DIR/build-host.sh"
[ -f "$LIBFFI_DIR/build-host/prefix/lib/libffi.a" ]   || sh "$LIBFFI_DIR/build-host.sh"
if [ ! -f "$ZLIB_PREFIX/include/zlib.h" ]; then
  log "zlib prefix missing -> building the vendored zlib-1.3.1 (l6-libs1 recipe)"
  [ -f "$TP/zlib-1.3.1/build.sh" ] || die "no zlib at $ZLIB_PREFIX and no $TP/zlib-1.3.1/build.sh to build it"
  sh "$TP/zlib-1.3.1/build.sh"
fi
[ -f "$ZLIB_PREFIX/include/zlib.h" ] || die "zlib still missing at $ZLIB_PREFIX"

[ -d "$SRC" ] || sh "$HERE/fetch.sh"

# Prefixes first in PKG_CONFIG_PATH so GLib resolves pcre2/libffi/zlib to the
# vendored static builds, never the system copies.
export PKG_CONFIG_PATH="$PCRE2_DIR/build-host/prefix/lib/pkgconfig:$LIBFFI_DIR/build-host/prefix/lib/pkgconfig:$ZLIB_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- 1. configure (meson) ----------------------------------------------------
if [ ! -f "$BUILD/build.ninja" ]; then
  log "meson setup (static, no nls/selinux/libmount/sysprof, no docs/tests)"
  meson setup "$BUILD" "$SRC" \
    --prefix="$PREFIX" \
    --libdir=lib \
    --buildtype=release \
    --wrap-mode=nofallback \
    -Ddefault_library=static \
    -Dc_args=-fPIC \
    -Dnls=disabled \
    -Dselinux=disabled \
    -Dlibmount=disabled \
    -Dsysprof=disabled \
    -Dxattr=true \
    -Dman-pages=disabled \
    -Ddocumentation=false \
    -Dintrospection=disabled \
    -Ddtrace=disabled \
    -Dsystemtap=disabled \
    -Dtests=false \
    -Dinstalled_tests=false
fi

# --- 2. build + install ------------------------------------------------------
ninja -C "$BUILD" -j "$JOBS"
ninja -C "$BUILD" install

log "-- installed static libs and pkg-config --"
ls -l "$PREFIX"/lib/*.a
ls "$PREFIX"/lib/pkgconfig/ | sort | tr '\n' ' '; echo

# --- 3. smoke test -----------------------------------------------------------
# The fresh GLib prefix must be on the pkg-config path for the smoke link.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
SMOKE_DIR="$HERE/build-host/smoke"
mkdir -p "$SMOKE_DIR"
GLIB_PCS="glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0"
# shellcheck disable=SC2086
"$CC" -O2 -Wall -Wextra \
  $(pkg-config --cflags $GLIB_PCS) \
  "$HERE/smoke/glib_smoke.c" \
  $(pkg-config --static --libs $GLIB_PCS) \
  -o "$SMOKE_DIR/glib_smoke"
"$SMOKE_DIR/glib_smoke" > "$HERE/build-host/SMOKE.RESULT"
cat "$HERE/build-host/SMOKE.RESULT"
log "smoke result -> $HERE/build-host/SMOKE.RESULT"
