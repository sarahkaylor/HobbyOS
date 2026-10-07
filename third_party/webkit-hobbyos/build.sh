#!/usr/bin/env bash
# build.sh — build the HobbyOS browser (WebProcess) FROM THE VENDORED WebKit
# fork source in this directory (third_party/webkit-hobbyos/, browser/rp-f
# @ 45349cb2).  Source-first: everything needed to build is in the repo
# snapshot; the committed cache/ binaries are optional shortcuts, never
# required.
#
#   bash third_party/webkit-hobbyos/build.sh [--arch arm|intel] [-j N]
#        [--fresh] [--build-deps] [--deps-only] [--configure-only]
#        [--source DIR] [--build-dir DIR] [--prefix DIR]
#        [--targets "WebProcess ..."]
#
# What it does:
#   1. extracts the vendored snapshot to src/ if not present (extract.sh;
#      sha-verified)
#   2. checks the cross-deps staging prefix; --build-deps runs the port's own
#      recipes (snapshot HobbyOS/scripts/wk2-libs-cross.sh; intel curl/mbedTLS
#      via the OS repo's tools/cross-intel-wk2-libs.sh)
#   3. configures with the canonical WK-2 recipe (mirrors
#      HobbyOS/scripts/wk2-link-ci.sh) against the extracted source —
#      -DWDK_FORK_DIR / -DHDYOS_SYSROOT / -DHOBBYOS_WK2_PREFIX are re-pointed
#      into this repo, never at a developer's home
#   4. ninja-builds the requested targets (default: WebProcess)
#   5. objcopy's the flat image to build/<arch>/browser.bin — the file
#      `make disk.img` picks up when no fork/worktree build is present — and
#      prints size + sha256 + a marker-symbol verification
#
# Prerequisites (see README.md "Dependencies"): the OS-side sysroot closure
# for the arch (obj/<arch>/{crt0.o,libc.a,libcxx.a[,setjmp.o]} and
# obj/<arch>/icu/libicu*.a) plus clang / ld.lld / llvm-objcopy / ninja / xz.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ARCH="arm"
JOBS="$(nproc 2>/dev/null || echo 8)"
SRC="$HERE/src"
FRESH=0
BUILD_DEPS=0
DEPS_ONLY=0
CONFIGURE_ONLY=0
TARGETS="WebProcess"
PREFIX_OVERRIDE=""
BUILD_OVERRIDE=""
CMAKE_BIN="${CMAKE:-$HOME/.local/share/l6-tools/cmake-3.31.8-linux-x86_64/bin/cmake}"
NINJA_BIN="${NINJA:-ninja}"
OBJCOPY_BIN="$(command -v llvm-objcopy || command -v objcopy || true)"

usage() {
  echo "usage: $0 [--arch arm|intel] [-j N] [--fresh] [--build-deps] [--deps-only]"
  echo "          [--configure-only] [--source DIR] [--build-dir DIR] [--prefix DIR]"
  echo "          [--targets \"WebProcess ...\"]"
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    -j|--jobs) JOBS="$2"; shift 2;;
    --fresh) FRESH=1; shift;;
    --build-deps) BUILD_DEPS=1; shift;;
    --deps-only) BUILD_DEPS=1; DEPS_ONLY=1; shift;;
    --configure-only) CONFIGURE_ONLY=1; shift;;
    --source) SRC="$2"; shift 2;;
    --build-dir) BUILD_OVERRIDE="$2"; shift 2;;
    --prefix) PREFIX_OVERRIDE="$2"; shift 2;;
    --targets) TARGETS="$2"; shift 2;;
    -h|--help) usage;;
    *) echo "unknown arg: $1" >&2; usage;;
  esac
done

case "$ARCH" in
  arm|intel) ;;
  *) echo "FATAL: --arch must be arm or intel (got '$ARCH')" >&2; exit 2;;
esac

BUILD="${BUILD_OVERRIDE:-$HERE/build/$ARCH/WebKitBuild}"
FLAT="$HERE/build/$ARCH/browser.bin"
PREFIX="${PREFIX_OVERRIDE:-$SRC-wk2/$ARCH/prefix}"

log() { printf '[browser-build:%s] %s\n' "$ARCH" "$*"; }

# 0. tool sanity ------------------------------------------------------------------
# Workstation toolchain location used by the port's own scripts (clang, lld,
# llvm-*); prepended BEFORE the checks so nothing depends on the login PATH.
export PATH="$HOME/.local/bin:/usr/lib/llvm-21/bin:$PATH"
OBJCOPY_BIN="$(command -v llvm-objcopy || command -v objcopy || true)"
[ -x "$CMAKE_BIN" ] || CMAKE_BIN="$(command -v cmake || true)"
[ -n "$CMAKE_BIN" ] || { echo "FATAL: cmake not found (set CMAKE=... or install the l6-tools cmake 3.31.8)" >&2; exit 1; }
for t in clang ld.lld "$NINJA_BIN" xz tar; do
  command -v "$t" >/dev/null || { echo "FATAL: missing tool: $t" >&2; exit 1; }
done
[ -n "$OBJCOPY_BIN" ] || { echo "FATAL: neither llvm-objcopy nor objcopy on PATH" >&2; exit 1; }

# 1. vendored source present? ------------------------------------------------------
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  log "vendored source not extracted yet — running extract.sh (sha-verified)"
  bash "$HERE/extract.sh" --dest "$SRC"
fi
[ -f "$SRC/HobbyOS/toolchain-hobbyos.cmake" ] || {
  echo "FATAL: $SRC/HobbyOS/toolchain-hobbyos.cmake missing (bad extraction?)" >&2; exit 1; }

# 2. cross-deps staging prefix -----------------------------------------------------
if [ "$BUILD_DEPS" = 1 ]; then
  log "building cross-deps -> $PREFIX   (wk2-libs-cross.sh; tarballs from $ROOT/third_party)"
  HDYOS="$ROOT" bash "$SRC/HobbyOS/scripts/wk2-libs-cross.sh" --arch "$ARCH" --prefix "$PREFIX" -j "$JOBS"
  if [ "$ARCH" = intel ]; then
    log "intel curl/mbedTLS staging (OS repo tools/cross-intel-wk2-libs.sh)"
    HOBBYOS_WK2_PREFIX="$PREFIX" FORK="$SRC" bash "$ROOT/tools/cross-intel-wk2-libs.sh" -j "$JOBS" || {
      echo "HINT: intel curl/mbedTLS staging failed — re-run manually:" >&2
      echo "  HOBBYOS_WK2_PREFIX=$PREFIX FORK=$SRC bash $ROOT/tools/cross-intel-wk2-libs.sh" >&2
    }
  fi
fi
CHECK_LIBS="libz.a libpng.a libjpeg.a libwebp.a libwebpdemux.a libwebpmux.a libfreetype.a libharfbuzz.a libsqlite3.a libxml2.a libcurl.a libmbedtls.a"
MISSING=""
for l in $CHECK_LIBS; do [ -f "$PREFIX/lib/$l" ] || MISSING="$MISSING $l"; done
if [ -n "$MISSING" ]; then
  echo "FATAL: cross-deps prefix incomplete: $PREFIX" >&2
  for l in $MISSING; do echo "  missing lib/$l" >&2; done
  echo "  run:  bash $0 --arch $ARCH --build-deps" >&2
  echo "  (ICU + the OS-side sysroot closure are separate prerequisites — see README.md)" >&2
  exit 1
fi
if [ "$DEPS_ONLY" = 1 ]; then
  log "deps-only: prefix OK at $PREFIX"
  exit 0
fi

# 3. OS-side sysroot closure -------------------------------------------------------
OBJ="$ROOT/obj/$ARCH"
NEED="crt0.o libc.a libcxx.a"
if [ "$ARCH" = intel ]; then NEED="$NEED setjmp.o"; fi
for o in $NEED; do
  [ -f "$OBJ/$o" ] || { echo "FATAL: $OBJ/$o missing — build the OS-side closure first (README.md 'Dependencies')" >&2; exit 1; }
done
[ -f "$OBJ/icu/libicuuc.a" ] || { echo "FATAL: $OBJ/icu/libicuuc.a missing (ICU cross build — README.md 'Dependencies')" >&2; exit 1; }

# 4. configure (canonical WK-2 recipe; mirrors HobbyOS/scripts/wk2-link-ci.sh) ------
if [ "$FRESH" = 1 ]; then
  log "fresh build dir: $BUILD"
  rm -rf "$BUILD"
fi
mkdir -p "$BUILD"
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  log "configure (log: $BUILD/configure.log)"
  if ! "$CMAKE_BIN" -S "$SRC" -B "$BUILD" -G Ninja \
      -DPORT=HobbyOS -DHOBBYOS_ARCH="$ARCH" \
      -DCMAKE_TOOLCHAIN_FILE="$SRC/HobbyOS/toolchain-hobbyos.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$(command -v "$NINJA_BIN")" \
      -DUSE_LCMS=OFF -DUSE_WOFF2=OFF -DUSE_JPEGXL=OFF -DUSE_AVIF=OFF \
      -DWDK_FORK_DIR="$SRC" -DHDYOS_SYSROOT="$ROOT" \
      -DHOBBYOS_WK2_PREFIX="$PREFIX" > "$BUILD/configure.log" 2>&1; then
    echo "FATAL: configure failed (tail of $BUILD/configure.log):" >&2
    tail -30 "$BUILD/configure.log" >&2 || true
    exit 1
  fi
  log "configure OK"
else
  log "reusing build dir (incremental): $BUILD"
fi
if [ "$CONFIGURE_ONLY" = 1 ]; then
  log "configure-only: OK ($BUILD) — no build run"
  exit 0
fi

# 5. build -------------------------------------------------------------------------
log "ninja -C build $TARGETS -j $JOBS"
"$NINJA_BIN" -C "$BUILD" $TARGETS -j "$JOBS"

# 6. flat image + verification -----------------------------------------------------
WP="$BUILD/bin/WebProcess"
[ -f "$WP" ] || { echo "FATAL: $WP missing after build" >&2; exit 1; }
MARKERS="$(grep -a -c 'WK5WindowDriver' "$WP" || true)"
if [ "${MARKERS:-0}" -eq 0 ]; then
  echo "FATAL: WK5WindowDriver marker not found in $WP — refusing to bless this build" >&2
  exit 1
fi
"$OBJCOPY_BIN" -O binary "$WP" "$FLAT"
log "WebProcess ELF: $WP"
log "  size $(stat -c%s "$WP") B  sha256 $(sha256sum "$WP" | cut -d' ' -f1)"
log "  WK5WindowDriver marker strings: $MARKERS"
log "flat image:     $FLAT ($(stat -c%s "$FLAT") B)"
log "flat sha256:    $(sha256sum "$FLAT" | cut -d' ' -f1)"
log "done.  'make disk.img' auto-uses the flat when no BROWSER_BIN/fork build is present;"
log "force this build with:  make disk.img BROWSER_BIN=$WP"
