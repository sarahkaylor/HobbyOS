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

# --- Concurrent-invocation guard ------------------------------------------
# The Makefile has four call sites for this script (.fetched-ok, the libc++
# header self-heal and two builtins self-heal guards), so one cold
# `make -j32` starts several of them AT ONCE.  They all downloaded into the
# same "${TARBALL}.part", so the first mv won and the others died with
# "mv: cannot stat ...llvm-project-21.1.8.src.tar.xz.part" (which killed the
# build at libcxx.a / the ICU manifest on a fresh checkout), and two
# concurrent tar extractions into the same $SRC can corrupt the tree.
# Serialize the whole body.  mkdir is atomic everywhere we build; macOS has
# no flock(1) by default, so do not use it.
LOCK_DIR="${VENDOR_DIR}/.fetch.lock.d"
LOCKED=0
WAITED=0
while [ "$WAITED" -lt 600 ]; do
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    LOCKED=1
    trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT
    break
  fi
  # Only the first waiter says why it is slow; a warm racer exits in ~1s.
  if [ "$WAITED" -eq 0 ]; then
    log "another fetch.sh holds ${LOCK_DIR}; waiting for it to finish"
  fi
  sleep 1
  WAITED=$((WAITED + 1))
done
if [ "$LOCKED" -ne 1 ]; then
  # A killed build (SIGKILL skips the trap) leaves the lock.  Do not steal it
  # blindly: if the tree the lock was protecting is complete, this run has
  # nothing to do; otherwise fail loudly instead of racing an extractor.
  if [ -f "$MARKER" ] && [ -d "$SRC/libcxx" ] && verify_anchors 2>/dev/null; then
    log "stale lock ${LOCK_DIR} but the tree verifies; removing it"
    rmdir "$LOCK_DIR" 2>/dev/null || true
    sync_overlay
    log "OK: ${SRC} ready (marker $(cat "$MARKER"))"
    exit 0
  fi
  {
    echo "[fetch] ERROR: ${LOCK_DIR} is stale and ${SRC} does not verify."
    echo "  Another fetch may still be running; otherwise remove the lock"
    echo "  directory and re-run."
  } >&2
  exit 1
fi

if [ -f "$MARKER" ] && [ -d "$SRC/libcxx" ] && verify_anchors 2>/dev/null; then
  log "tree present + anchors verified; re-syncing config overlay"
  sync_overlay
  log "OK: ${SRC} ready (marker $(cat "$MARKER"))"
  exit 0
fi

if [ ! -f "$TARBALL" ]; then
  log "downloading ${URL}"
  # Unique temp name as well as the lock: a human may run fetch.sh by hand
  # while a build is fetching, and a shared .part would repeat the clobber.
  PART="${TARBALL}.part.$$"
  curl -fL --retry 3 -o "$PART" "$URL"
  mv -f "$PART" "$TARBALL"
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
