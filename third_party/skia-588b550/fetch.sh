#!/usr/bin/env bash
# fetch.sh -- reconstruct the pinned Skia source tree for the L6 host recipe.
#
# Pin (browser.md §2 / W0.4 audit; docs/browser/f0-deps-audit.md §2):
#   upstream : https://skia.googlesource.com/skia
#   commit   : 588b550a4dd8af90dbe71c0554852806bd8f0b21
#              (the revision WebKit 2.54.0 vendors; see
#               webkit/README.WebKit -- "Commit: 588b550a4dd8...";
#               fork clone committed at 5220e80b97 "2.54.0 release")
#   overlay  : webkit/{CMakeLists.txt,WebKitSkiaConfig.h,README.WebKit}
#              (WebKit-authored files; hashed in webkit/SHA256SUMS)
#
# Integrity model: googlesource "+archive" gzip containers are NOT
# byte-reproducible (two downloads of the same commit, 2026-09-30, gave
# sha256 307628b6... / 3f2de0db... --- the container sha is recorded but is
# NOT the pin).  The gate is a canonical CONTENT manifest of the extracted
# tree (see manifest.py):
#   manifest_sha256=c27786cade6336a16e284fc9ccf09a137eaba3684ddaf94d9414aab4d0d47213
#   12395 entries (12394 files + 1 symlink), excludes the 3 overlay names.
#
# Cross-checked 2026-09-30 against the WebKit side (read-only):
#   WPE 2.54.0 release tarball Source/ThirdParty/skia: 3059 files, all of
#   them byte-identical to this tree; fork clone Source/ThirdParty/skia:
#   12389 files = this tree minus 9 package-lock/gradle files WebKit's
#   ignore rules drop, plus the 3 WebKit files.  (See README.md.)
#
# Usage: ./fetch.sh
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REV="588b550a4dd8af90dbe71c0554852806bd8f0b21"
URL="https://skia.googlesource.com/skia/+archive/${REV}.tar.gz"
TARBALL="${VENDOR_DIR}/skia-${REV}.tar.gz"
SRC="${VENDOR_DIR}/src"
MARKER="${VENDOR_DIR}/.fetched-ok"
EXPECT_MANIFEST="c27786cade6336a16e284fc9ccf09a137eaba3684ddaf94d9414aab4d0d47213"
EXPECT_FILES=12395
EXCLUDES=(--exclude-path CMakeLists.txt --exclude-path WebKitSkiaConfig.h --exclude-path README.WebKit)

log() { printf '[fetch] %s\n' "$*"; }

verify_tree() {
  local out got gotfiles
  out="$(python3 "${VENDOR_DIR}/manifest.py" "$SRC" "${EXCLUDES[@]}")"
  echo "$out"
  got="${out#manifest_sha256=}"
  got="${got%% *}"
  gotfiles="$(echo "$out" | sed -n 's/.*files=\([0-9]*\).*/\1/p')"
  if [ "$got" != "$EXPECT_MANIFEST" ] || [ "$gotfiles" != "$EXPECT_FILES" ]; then
    return 1
  fi
  return 0
}

if [ -f "$MARKER" ] && [ "$(cat "$MARKER")" = "$EXPECT_MANIFEST/$EXPECT_FILES" ]; then
  log "tree marker present; re-verifying content (cheap; catches local pollution)"
  if verify_tree; then
    log "tree still content-verified ($SRC); nothing to do"
    exit 0
  fi
  cat >&2 <<'EOF'
[fetch] ERROR: the extracted tree no longer matches the pin -- something wrote
        into src/ (probes, editors, builds).  The tree must stay pristine so
        the content gate means anything.  Move the foreign files OUT of src/
        and re-run fetch.sh (a re-extract cannot remove extra files safely).
EOF
  exit 1
fi

if [ ! -f "$TARBALL" ]; then
  log "downloading ${URL}"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
else
  log "reusing existing container ${TARBALL}"
fi
log "container sha256 (informational, not the pin): $(sha256sum "$TARBALL" | cut -d' ' -f1)"

log "extracting into ${SRC}"
mkdir -p "$SRC"
tar -xzf "$TARBALL" -C "$SRC"

log "verifying content manifest (the pin gate)"
if ! verify_tree; then
  {
    echo "[fetch] ERROR: content manifest mismatch -- refusing to build."
    echo "  expected $EXPECT_MANIFEST ($EXPECT_FILES entries)"
    echo "  If upstream rewrote history this pin must be re-audited; do NOT eyeball past this."
  } >&2
  exit 1
fi

log "overlaying the WebKit-authored files (hashes from webkit/SHA256SUMS)"
( cd "${VENDOR_DIR}/webkit" && sha256sum -c SHA256SUMS )
cp "${VENDOR_DIR}/webkit/CMakeLists.txt" "${SRC}/CMakeLists.txt"
cp "${VENDOR_DIR}/webkit/WebKitSkiaConfig.h" "${SRC}/WebKitSkiaConfig.h"
cp "${VENDOR_DIR}/webkit/README.WebKit" "${SRC}/README.WebKit"

printf '%s' "$EXPECT_MANIFEST/$EXPECT_FILES" > "$MARKER"
log "OK: ${SRC} ready (${EXPECT_FILES} upstream entries + 3 WebKit files)"
