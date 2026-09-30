#!/bin/sh
# fetch.sh -- reconstruct the pinned libffi source tree for the L6 host recipe.
#
# Pin: libffi 3.4.8 (final release of the long-stable 3.4 series, 2025-04-10;
# lane directive: "libffi 3.4.x").  Newer stable lines exist upstream (3.5.x ->
# 3.8.0, and GLib's own subprojects/libffi.wrap pins the wrapdb build of 3.5.2)
# but GLib's floor is only `libffi >= 3.0.0`
# (glib-2.88.3/meson.build:2301) and 3.4.8 is the version with the widest
# distro/CI exposure; recorded in README.md as a deliberate, revisitable pin.
#
# Why libffi here: GLib's GObject closures (g_cclosure_marshal_generic, the
# fallback for every `g_signal_connect`-style closure WPE will register) are
# libffi-backed; GLib treats it as a hard dependency (dependency() with no
# required:False).
#
# Integrity model:
#   upstream : https://github.com/libffi/libffi/releases/tag/v3.4.8
#   tarball  : libffi-3.4.8.tar.gz (1,397,992 B; the only release asset)
#   sha256   : `bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b`
#              (recorded in libffi-3.4.8.tar.gz.sha256)
#   provenance: upstream publishes no .sha256/.sig for this asset; the recorded
#              hash was recomputed locally (2026-09-30) and cross-checked against
#              Gentoo's dev-libs/libffi Manifest (SHA512
#              05344c6c1a1a5b44704f6cf99277098d1ea3ac1dc11c2a691c501786a214f76184ec0637135588630db609ce79e49df3dbd00282dd61e7f21137afba70e24ffe,
#              size 1397992 -- both match) and the Debian trixie source package
#              (`libffi 3.4.8-2`, sources.debian.org).
#
# Usage: sh fetch.sh   (idempotent; downloads + verifies + extracts into src/)
set -eu

VENDOR_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
TARBALL="${VENDOR_DIR}/libffi-3.4.8.tar.gz"
SUMS="$(basename "${VENDOR_DIR}/libffi-3.4.8.tar.gz.sha256")"
SRC="${VENDOR_DIR}/src"
MARKER="${VENDOR_DIR}/.fetched-ok"
URL="https://github.com/libffi/libffi/releases/download/v3.4.8/libffi-3.4.8.tar.gz"

log() { printf '[fetch] %s\n' "$*"; }

if [ ! -f "$TARBALL" ]; then
  log "downloading $URL"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
else
  log "reusing existing ${TARBALL}"
fi

log "verifying sha256 (gate = the committed .sha256 record)"
( cd "$VENDOR_DIR" && sha256sum -c "$SUMS" )

if [ -f "$MARKER" ]; then
  log "extraction marker present; nothing to do (${SRC})"
  exit 0
fi

log "extracting into ${SRC}"
mkdir -p "$SRC"
tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
printf 'sha256:%s\n' "$(cut -d' ' -f1 "$VENDOR_DIR/libffi-3.4.8.tar.gz.sha256")" > "$MARKER"
log "OK: ${SRC} ready (remove src/ and re-run if a partial extraction ever happens)"
