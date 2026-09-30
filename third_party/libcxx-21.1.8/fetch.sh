#!/usr/bin/env bash
# fetch.sh -- reconstruct the pinned libc++ 21.1.8 source tree for the P3
# static port (browser.md §6 P3.1).  Same vendor recipe as
# third_party/skia-588b550/.
#
# Pin (release tag, not a floating branch):
#   upstream : https://github.com/llvm/llvm-project/releases
#   tag      : llvmorg-21.1.8
#   container: llvm-project-21.1.8.src.tar.xz
#              sha256 4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142
#              (GitHub release assets are content-stable; the container sha
#               below IS the raw-download pin.  Verified twice on 2026-09-30,
#               including after the integrator's "process died" heads-up.)
#
# Only the subtrees the port needs are extracted (the monorepo tarball is
# ~1.4 GiB unpacked; the libcxx/ subtree alone carries its test suite):
#   libcxx/                     the C++ standard library
#   libcxxabi/                  the Itanium C++ ABI runtime
#   libc/shared/                LLVM-libc shared headers that libc++'s
#                               <charconv> floating-point code includes
#                               ("shared/fp_bits.h", "shared/str_to_float.h")
#   compiler-rt/lib/builtins/   aarch64 binary128 (long double) helpers
#                               (__extenddftf2 et al; x86_64 needs none)
#   cmake/                      upstream CMake glue kept for reference
#   LICENSE.TXT                 Apache-2.0 WITH LLVM-exception
#
# Integrity model: container sha256 (raw download pin) plus an anchor
# manifest of four untouched upstream files (anchors.sha256) that is
# re-verified on every run.  The libc++ build additionally writes
# upstream-configure-time files (__config_site, __assertion_handler) from
# the committed config/ dir -- those are overlays, not upstream, and are
# re-synced here so the in-tree copies always match what is committed.
#
# Usage: ./fetch.sh
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="llvmorg-21.1.8"
VER="21.1.8"
URL="https://github.com/llvm/llvm-project/releases/download/${TAG}/llvm-project-${VER}.src.tar.xz"
TARBALL="${VENDOR_DIR}/llvm-project-${VER}.src.tar.xz"
SRC="${VENDOR_DIR}/src/llvm-project-${VER}.src"
MARKER="${VENDOR_DIR}/.fetched-ok"
EXPECT_SHA="$(cut -d' ' -f1 "${VENDOR_DIR}/container.sha256")"
SUBTREES=(
  "llvm-project-${VER}.src/libcxx"
  "llvm-project-${VER}.src/libcxxabi"
  "llvm-project-${VER}.src/cmake"
  "llvm-project-${VER}.src/LICENSE.TXT"
  "llvm-project-${VER}.src/libc/shared"
  "llvm-project-${VER}.src/compiler-rt/lib/builtins"
)

log() { printf '[fetch] %s\n' "$*"; }

verify_anchors() {
  ( cd "$VENDOR_DIR" && sha256sum -c --quiet anchors.sha256 )
}

sync_overlay() {
  # __config_site / __assertion_handler are CMake configure_file outputs
  # upstream; the port hand-writes them (no CMake in the loop) and commits
  # them under config/.  Always re-copy: cheap, and it is the one way the
  # in-tree include dir can legitimately differ from the raw tarball.
  cp "${VENDOR_DIR}/config/__config_site" "${SRC}/libcxx/include/__config_site"
  cp "${VENDOR_DIR}/config/__assertion_handler" "${SRC}/libcxx/include/__assertion_handler"
}

if [ -f "$MARKER" ] && [ -d "$SRC/libcxx" ] && verify_anchors 2>/dev/null; then
  log "tree present + anchors verified; re-syncing config overlay"
  sync_overlay
  log "OK: ${SRC} ready (marker $(cat "$MARKER"))"
  exit 0
fi

if [ ! -f "$TARBALL" ]; then
  log "downloading ${URL}"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
else
  log "reusing existing container ${TARBALL}"
fi

got="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
if [ "$got" != "$EXPECT_SHA" ]; then
  {
    echo "[fetch] ERROR: container sha256 mismatch -- refusing to build."
    echo "  expected $EXPECT_SHA"
    echo "  got      $got"
    echo "  (If GitHub re-cut the release asset this pin must be re-audited;"
    echo "   do NOT eyeball past this.)"
  } >&2
  exit 1
fi
log "container sha256 OK"

log "extracting the needed subtrees into ${SRC}"
mkdir -p "$SRC"
tar -xJf "$TARBALL" -C "$SRC" --strip-components=1 "${SUBTREES[@]}"

log "verifying anchor manifest (the content gate)"
if ! verify_anchors; then
  {
    echo "[fetch] ERROR: anchor manifest mismatch in the extracted tree."
    echo "  The tarball cannot produce a different content hash for these"
    echo "  files; something wrote into src/.  Re-extract (do not patch"
    echo "  these in place; use config/ for overlays)."
  } >&2
  exit 1
fi

sync_overlay

printf 'llvmorg-%s' "$VER" > "$MARKER"
log "OK: ${SRC} ready (libcxx + libcxxabi + libc/shared + compiler-rt builtins)"
