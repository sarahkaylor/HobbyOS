#!/usr/bin/env bash
# fetch.sh -- reconstruct the pinned LLVM/Clang 21.1.8 cross-build source tree
# for the HobbyOS self-hosting toolchain (selfhost.md §2/AD-3; lane L1).
#
# Pin (release tag, not a floating branch):
#   upstream : https://github.com/llvm/llvm-project/releases
#   tag      : llvmorg-21.1.8
#   container: llvm-project-21.1.8.src.tar.xz
#              sha256 4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142
#              (the FULL monorepo release: llvm/, clang/, lld/, cmake/, ...)
#
# Container reuse: the SAME container is already vendored by the P3 libc++
# vendor at third_party/libcxx-21.1.8/llvm-project-21.1.8.src.tar.xz.  If that
# file exists and its sha256 matches the pin, we reuse it (no duplicate
# ~152 MB download); fetch.sh re-verifies the sha BEFORE any use either way.
#
# Subtrees extracted (what the toolchain build needs; the monorepo tarball
# unpacks to ~1.4 GiB and contains 15+ projects -- only these are pulled):
#   llvm/                       the LLVM core: Support, Object, TableGen,
#                               llvm-ar, llvm-objcopy sources
#   clang/                      the C/C++ compiler driver + frontend
#   lld/                        the ELF/COFF/Mach-O linker (ld.lld)
#   cmake/                      upstream CMake glue (Modules, config)
#   third-party/                monorepo-root vendored deps LLVM builds link
#                               (siphash via LLVM_THIRD_PARTY_DIR; benchmark/
#                               unittest present for parity)
#   LICENSE.TXT                 Apache-2.0 WITH LLVM-exception
#
# Integrity model: container sha256 (raw download pin) plus an anchor
# manifest of untouched upstream files (anchors.sha256) that is re-verified
# on every run, exactly like third_party/libcxx-21.1.8/.
#
# Usage: ./fetch.sh
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBCXX_VENDOR_DIR="$(cd "${VENDOR_DIR}/../libcxx-21.1.8" && pwd)"
TAG="llvmorg-21.1.8"
VER="21.1.8"
URL="https://github.com/llvm/llvm-project/releases/download/${TAG}/llvm-project-${VER}.src.tar.xz"
TARBALL="${VENDOR_DIR}/llvm-project-${VER}.src.tar.xz"
SRC="${VENDOR_DIR}/src/llvm-project-${VER}.src"
MARKER="${VENDOR_DIR}/.fetched-ok"
EXPECT_SHA="$(cut -d' ' -f1 "${VENDOR_DIR}/container.sha256")"
SUBTREES=(
  "llvm-project-${VER}.src/llvm"
  "llvm-project-${VER}.src/clang"
  "llvm-project-${VER}.src/lld"
  "llvm-project-${VER}.src/cmake"
  "llvm-project-${VER}.src/third-party"
  "llvm-project-${VER}.src/libunwind/include"
  "llvm-project-${VER}.src/LICENSE.TXT"
)

log() { printf '[fetch] %s\n' "$*"; }

verify_anchors() {
  ( cd "$VENDOR_DIR" && sha256sum -c --quiet anchors.sha256 )
}

if [ -f "$MARKER" ] && [ -d "$SRC/llvm" ] && [ -d "$SRC/clang" ] && \
   [ -d "$SRC/lld" ] && verify_anchors 2>/dev/null; then
  log "tree present + anchors verified; nothing to do"
  log "OK: ${SRC} ready (marker $(cat "$MARKER"))"
  exit 0
fi

# Prefer the libcxx vendor's container (it IS the same pinned file); fall
# back to a download into this vendor dir.
if [ -f "$TARBALL" ]; then
  log "reusing existing container ${TARBALL}"
elif [ -f "${LIBCXX_VENDOR_DIR}/llvm-project-${VER}.src.tar.xz" ] &&
     [ "$(sha256sum "${LIBCXX_VENDOR_DIR}/llvm-project-${VER}.src.tar.xz" | cut -d' ' -f1)" = "$EXPECT_SHA" ]; then
  log "reusing the libcxx vendor's sha-verified container"
  cp "${LIBCXX_VENDOR_DIR}/llvm-project-${VER}.src.tar.xz" "$TARBALL"
else
  log "downloading ${URL}"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
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
    echo "  these in place)."
  } >&2
  exit 1
fi

printf 'llvmorg-%s' "$VER" > "$MARKER"
log "OK: ${SRC} ready (llvm + clang + lld + cmake + LICENSE.TXT)"
