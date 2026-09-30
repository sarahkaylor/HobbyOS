#!/usr/bin/env bash
# fetch.sh -- reconstruct the pinned GLib source tree for the L6 host recipe.
#
# Pin: GLib 2.88.3 (newest stable of the 2.8x line as of 2026-09-30; released
# 2026-07-29).  WPE 2.54.0 hard-requires GLib >= 2.70.0
# (Source/cmake/OptionsWPE.cmake:12, quoted in docs/browser/f0-deps-audit.md
# sec 1.1; browser.md sec 1.4 / W0.4 note adds "GLib (+deps) is added to the L6
# host-first build list").  2.88.3 carries the CVE-2026-15588 fix
# (GDBusServer pre-auth DoS) and a GCC 17 miscompilation fix; 2.86.5 is the
# older stable alternative (sha256 recorded in the README as the fallback).
#
# Host-first build only -- not wired into the HobbyOS Makefile; build-host.sh is
# the recipe the WPE/port cross builds reuse (see cross-notes.md).
#
# Integrity model:
#   upstream : https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz
#   tarball  : glib-2.88.3.tar.xz (5,794,356 B)
#   sha256   : `ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b`
#              (recorded in glib-2.88.3.tar.xz.sha256).  This is the *official*
#              value published by GNOME in the release directory's
#              glib-2.88.3.sha256sum file; the local download was recomputed and
#              matches it byte-for-byte.
#
# Usage: ./fetch.sh   (idempotent; downloads + verifies + extracts into src/)
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARBALL="${VENDOR_DIR}/glib-2.88.3.tar.xz"
SUMS="$(basename "${VENDOR_DIR}/glib-2.88.3.tar.xz.sha256")"
SRC="${VENDOR_DIR}/src"
MARKER="${VENDOR_DIR}/.fetched-ok"
URL="https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz"

log() { printf '[fetch] %s\n' "$*"; }

if [ ! -f "$TARBALL" ]; then
  log "downloading $URL"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
else
  log "reusing existing ${TARBALL}"
fi

log "verifying sha256 (gate = the committed .sha256 record; official GNOME sum)"
( cd "$VENDOR_DIR" && sha256sum -c "$SUMS" )

if [ -f "$MARKER" ]; then
  log "extraction marker present; nothing to do (${SRC})"
  exit 0
fi

log "extracting into ${SRC}"
mkdir -p "$SRC"
tar -xJf "$TARBALL" -C "$SRC" --strip-components=1
printf 'sha256:%s\n' "$(cut -d' ' -f1 "$VENDOR_DIR/glib-2.88.3.tar.xz.sha256")" > "$MARKER"
log "OK: ${SRC} ready (remove src/ and re-run if a partial extraction ever happens)"
