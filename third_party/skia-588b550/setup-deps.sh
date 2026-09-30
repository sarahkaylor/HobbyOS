#!/usr/bin/env bash
# setup-deps.sh -- rootless host toolchain deps for the Skia host build.
#
# fontconfig is required by the WPE-faithful font-manager path
# (webkit/CMakeLists.txt:6 "find_package(Fontconfig 2.13.0 REQUIRED)").
# Ubuntu 26.04 ships the runtime (libfontconfig.so.1) but not the headers, and
# we have no sudo: extract the dev package with `apt-get download` +
# `dpkg-deb -x` into a local prefix (same rootless pattern as the L6 cmake
# toolchain at ~/.local/share/l6-tools/).
#
#   deb     : libfontconfig-dev_2.17.1-3ubuntu1_amd64.deb  (166 kB)
#   sha256  : cdb2383bd9c5fcdb0809943ecd21b11cb2da60777d345f052eda7672c1718e7c
#   source  : http://us.archive.ubuntu.com/ubuntu/pool/main/f/fontconfig/
#
# Also prints the sibling-lane dependency prefixes the build links against.
# Usage: ./setup-deps.sh          (FONTCONFIG_PREFIX overridable)
set -euo pipefail

FONTCONFIG_PREFIX="${FONTCONFIG_PREFIX:-$HOME/.local/share/l6-tools/fontconfig-2.17.1}"
DEB="libfontconfig-dev_2.17.1-3ubuntu1_amd64.deb"
DEB_SHA256="cdb2383bd9c5fcdb0809943ecd21b11cb2da60777d345f052eda7672c1718e7c"
WORK="${FONTCONFIG_PREFIX}/.deb-cache"

log() { printf '[deps] %s\n' "$*"; }

if [ -f "${FONTCONFIG_PREFIX}/include/fontconfig/fontconfig.h" ] && [ -e "${FONTCONFIG_PREFIX}/lib/libfontconfig.so" ]; then
  log "fontconfig prefix already prepared: ${FONTCONFIG_PREFIX}"
else
  command -v apt-get >/dev/null || { echo "[deps] ERROR: apt-get missing; prepare fontconfig headers at $FONTCONFIG_PREFIX manually" >&2; exit 1; }
  lib="$(ldconfig -p | awk '/libfontconfig\.so\.1 / && /x86_64/ {print $NF; exit}')"
  [ -n "$lib" ] || { echo "[deps] ERROR: libfontconfig.so.1 (runtime) not found; use SKIA_FONTMGR=empty to skip fontconfig" >&2; exit 1; }

  mkdir -p "$WORK" "${FONTCONFIG_PREFIX}/include" "${FONTCONFIG_PREFIX}/lib/pkgconfig"
  ( cd "$WORK"
    if [ ! -f "$DEB" ]; then
      apt-get download "libfontconfig-dev=2.17.1-3ubuntu1"
    fi
    echo "$DEB_SHA256  $DEB" | sha256sum -c -
    ext="extract.$$"
    mkdir -p "$ext"
    dpkg-deb -x "$DEB" "$ext" )
  cp -a "$WORK"/extract.*/usr/include/fontconfig "${FONTCONFIG_PREFIX}/include/"
  ln -sf "$lib" "${FONTCONFIG_PREFIX}/lib/libfontconfig.so"
  cat > "${FONTCONFIG_PREFIX}/lib/pkgconfig/fontconfig.pc" <<EOF
prefix=${FONTCONFIG_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Fontconfig
Description: Font configuration and customization library
Version: 2.17.1
Requires: freetype2 >= 21.0.15
Libs: -L\${libdir} -lfontconfig
Cflags: -I\${includedir}
EOF
  log "prepared ${FONTCONFIG_PREFIX} (headers from ${DEB}, lib -> ${lib})"
fi

log "host dependency prefixes this recipe links against (env-overridable):"
log "  FREETYPE_PREFIX  ${FREETYPE_PREFIX:-$HOME/hobbyos-lanes/l5-fonts/obj/third_party/freetype/prefix}"
log "  HARFBUZZ_PREFIX  ${HARFBUZZ_PREFIX:-$HOME/hobbyos-lanes/l5-fonts/obj/third_party/harfbuzz/prefix}"
log "  ZLIB_PREFIX      ${ZLIB_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/zlib-1.3.1/build-host/prefix}"
log "  PNG_PREFIX       ${PNG_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libpng-1.6.44/build-host/prefix}"
log "  JPEG_PREFIX      ${JPEG_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libjpeg-turbo-3.1.0/build-host/prefix}"
log "  WEBP_PREFIX      ${WEBP_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libwebp-1.6.0/build-host/prefix}"
log "  FONTCONFIG_PREFIX ${FONTCONFIG_PREFIX}"
log "  (expat comes from the system; pkg-config expat 2.7.4 on this host)"
