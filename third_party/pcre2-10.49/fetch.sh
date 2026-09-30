#!/usr/bin/env bash
# fetch.sh -- reconstruct the pinned pcre2 source tree for the L6 host recipe.
#
# Pin: pcre2 10.49 (PCRE2 10.4x line, newest stable).  2026-09-28 security-only
# release fixing GHSA-r9hj-j2rw-4q3m (OOB write when JIT stack handling is used
# with pathological patterns; "Users should upgrade to 10.49", affects <=10.48)
# -- so 10.48 and older are NOT valid pins for the browser stack.
#
# Why pcre2 here: WPE 2.54.0 gets regex/pcre via GLib, which hard-requires
# libpcre2-8 >= 10.32 (glib-2.88.3/meson.build:2252 `pcre2_req = '>=10.32'`,
# quoted in docs/browser/f0-deps-audit.md sec 1.1 GLib row); WebCore's GRegex
# usage is PCRE2-backed (glib/gregex.c).  Static-only build (see build-host.sh).
#
# Integrity model:
#   upstream : https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49
#   tarball  : pcre2-10.49.tar.bz2 (official release asset; GPG-signed)
#   sha256   : `53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384`
#              (recorded in pcre2-10.49.tar.bz2.sha256).  Upstream publishes no
#              .sha256 file; the record was cross-checked against the Homebrew
#              formula (homebrew-core master Formula/p/pcre2.rb pins the same
#              .tar.bz2 with the same sha256).
#   GPG      : pcre2-10.49.tar.bz2.sig (committed, 566 B) — verified with the
#              official release key documented in PCRE2 SECURITY.md:
#                Nicholas Wilson  A955 3620 4A3B B489 7152 3128 2A98 E77E B6F2 4CA8
#              result (2026-09-30): `gpg --verify` = "Good signature from
#              Nicholas Wilson <nicholas@nicholaswilson.me.uk>" (RSA subkey
#              BACF 71F1 0404 D576 1C09 D392 021D E40B FB63 B406).
#
# Usage: ./fetch.sh   (idempotent; downloads + verifies + extracts into src/)
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARBALL="${VENDOR_DIR}/pcre2-10.49.tar.bz2"
SUMS="$(basename "${VENDOR_DIR}/pcre2-10.49.tar.bz2.sha256")"
SRC="${VENDOR_DIR}/src"
MARKER="${VENDOR_DIR}/.fetched-ok"
URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.49/pcre2-10.49.tar.bz2"

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
tar -xjf "$TARBALL" -C "$SRC" --strip-components=1
printf 'sha256:%s\n' "$(cut -d' ' -f1 "$VENDOR_DIR/pcre2-10.49.tar.bz2.sha256")" > "$MARKER"
log "OK: ${SRC} ready (remove src/ and re-run if a partial extraction ever happens)"
