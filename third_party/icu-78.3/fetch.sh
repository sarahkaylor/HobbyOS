#!/usr/bin/env bash
# fetch.sh -- download + verify + extract the pinned ICU4C 78.3 source tarball
# for third_party/icu-78.3 (L6 lane browser/l6-icu; browser.md §2 ICU pin).
#
# Pin: icu4c-78.3-sources.tgz, released 2026-03-17 — newest *stable* ICU4C
# release satisfying WPE 2.54.0's `find_package(ICU 70.1 REQUIRED COMPONENTS
# data i18n uc)` floor (no upper bound; see README.md, "Version choice").
#
# Digests (cross-checked 2026-09-30 against three independently published
# values; see README.md, "Provenance"):
#   sha256 3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0
#          (GitHub release-asset digest for release-78.3)
#   sha512 04a49455e1489030c520a4bfd2664fa2171e7938d08f2acdbbcb1fda976639fd
#          8b1f0704f2eec89ba59a7b6d118ceaab6ec5a096e40d9085a0895d91ce225245
#          (upstream SHASUM512.txt, committed)
#   md5    a7b736b570ef0e180c96a31715a00c78
#          (upstream icu4c-78.3-sources.md5, committed)
#
# Committed: this script, the digest records (icu4c-78.3-sources.tgz.sha256,
# icu4c-78.3-sources.md5, SHASUM512.txt), data-filter-en.json, README.md,
# build-host.sh, build-host-trimmed.sh, smoke/.
# The 27 MB sources tarball, the 20 MB data-sources zip, the extracted src/
# and build-host/ are rebuilt by this script / the build scripts (gitignored).
#
# Usage: ./fetch.sh [--data-src]
#   --data-src   also download + verify icu4c-78.3-data.zip (the official
#                ICU data *sources*: data/locales/*.txt etc; NOT in the
#                sources tarball) and overlay it into src/source/data/.
#                Required to build data from source or to apply
#                ICU_DATA_FILTER_FILE trimming (build-host-trimmed.sh).
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VER=78.3
NAME="icu4c-${VER}-sources.tgz"
URL="https://github.com/unicode-org/icu/releases/download/release-${VER}/${NAME}"
TARBALL="$VENDOR_DIR/$NAME"
SRC="$VENDOR_DIR/src"

EXPECT_SHA256=3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0
EXPECT_SHA512=04a49455e1489030c520a4bfd2664fa2171e7938d08f2acdbbcb1fda976639fd8b1f0704f2eec89ba59a7b6d118ceaab6ec5a096e40d9085a0895d91ce225245
EXPECT_MD5=a7b736b570ef0e180c96a31715a00c78

DATA_NAME="icu4c-${VER}-data.zip"
DATA_URL="https://github.com/unicode-org/icu/releases/download/release-${VER}/${DATA_NAME}"
EXPECT_DATA_SHA256=9d8b3899096aeb83e4e21ef8a40fec9e03b28db18c48452efac882ce25a91e27

FETCH_DATA_SRC=0
for arg in "$@"; do
  case "$arg" in
    --data-src) FETCH_DATA_SRC=1 ;;
    *) echo "[fetch] unknown argument: $arg" >&2; exit 2 ;;
  esac
done

log() { printf '[fetch] %s\n' "$*"; }

if [ ! -f "$TARBALL" ]; then
  log "downloading ${URL}"
  curl -fL --retry 3 -o "${TARBALL}.part" "$URL"
  mv "${TARBALL}.part" "$TARBALL"
else
  log "reusing existing container ${TARBALL}"
fi

log "verifying digests (sha256 + sha512 + md5)"
got_sha256="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
got_sha512="$(sha512sum "$TARBALL" | cut -d' ' -f1)"
got_md5="$(md5sum "$TARBALL" | cut -d' ' -f1)"
[ "$got_sha256" = "$EXPECT_SHA256" ] || {
  echo "[fetch] ERROR: sha256 ${got_sha256} != expected ${EXPECT_SHA256}" >&2
  exit 1
}
[ "$got_sha512" = "$EXPECT_SHA512" ] || {
  echo "[fetch] ERROR: sha512 ${got_sha512} != expected ${EXPECT_SHA512}" >&2
  exit 1
}
[ "$got_md5" = "$EXPECT_MD5" ] || {
  echo "[fetch] ERROR: md5 ${got_md5} != expected ${EXPECT_MD5}" >&2
  exit 1
}
# Re-derive the upstream-published digests from the committed records too.
if [ -f "$VENDOR_DIR/icu4c-${VER}-sources.md5" ]; then
  ( cd "$VENDOR_DIR" && grep -F "$NAME" "icu4c-${VER}-sources.md5" | md5sum -c - )
fi
if [ -f "$VENDOR_DIR/SHASUM512.txt" ]; then
  ( cd "$VENDOR_DIR" && grep -F "$NAME" SHASUM512.txt | sha512sum -c - )
fi

if [ ! -f "$SRC/source/configure" ]; then
  log "extracting into ${SRC}"
  mkdir -p "$SRC"
  tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
else
  log "extracted tree present (${SRC}); skipping (delete it to re-extract)"
fi

if [ "$FETCH_DATA_SRC" = 1 ]; then
  DATA_ZIP="$VENDOR_DIR/$DATA_NAME"
  if [ ! -f "$DATA_ZIP" ]; then
    log "downloading ${DATA_URL}"
    curl -fL --retry 3 -o "${DATA_ZIP}.part" "$DATA_URL"
    mv "${DATA_ZIP}.part" "$DATA_ZIP"
  else
    log "reusing existing container ${DATA_ZIP}"
  fi
  got_data="$(sha256sum "$DATA_ZIP" | cut -d' ' -f1)"
  [ "$got_data" = "$EXPECT_DATA_SHA256" ] || {
    echo "[fetch] ERROR: data.zip sha256 ${got_data} != expected ${EXPECT_DATA_SHA256}" >&2
    exit 1
  }
  if [ ! -f "$SRC/source/data/locales/root.txt" ]; then
    log "overlaying ICU data sources (${DATA_NAME}) into ${SRC}/source/data"
    unzip -q -o "$DATA_ZIP" -d "$SRC/source/"
  else
    log "data sources already overlaid (${SRC}/source/data/locales/root.txt present)"
  fi
  log "note: to actually build data from source, data/in/icudt${VER%%.*}l.dat must"
  log "      be absent (ICU's make build uses the prebuilt archive whenever it"
  log "      exists and then ignores ICU_DATA_FILTER_FILE). build-host-trimmed.sh"
  log "      parks it; a plain re-extract of src/ restores it."
fi

log "OK: ICU ${VER} pin verified; source tree at ${SRC}"
