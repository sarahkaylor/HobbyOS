#!/usr/bin/env bash
# rebuild_browser.sh — regenerate the optional binary cache
# (cache/browser-{arm,x64}.bin.xz) from the vendored WebKit snapshot in this
# directory.  Source-first: the cache is a convenience artifact that this
# script rebuilds from third_party source; it is never a requirement.
#
#   bash third_party/webkit-hobbyos/rebuild_browser.sh [--arch arm|x64|both] [-j N]
#        [--build-deps] [--prefix-arm DIR] [--prefix-x64 DIR] [--from-existing]
#
# For each requested architecture:
#   1. runs build.sh (extract -> cross-deps -> configure -> ninja -> flat
#      build/<arch>/browser.bin, with WK5WindowDriver marker + sha checks)
#   2. re-compresses the flat with the committed settings (xz -T0 -6, CRC64)
#      into cache/browser-<name>.bin.xz  (arm -> browser-arm, x64 -> browser-x64;
#      build.sh's "intel" arch is mapped internally)
#   3. rewrites the cache lines in SHA256SUMS
#   4. prints the new flat/cache hashes; commit cache/ + SHA256SUMS to publish
#
# Notes:
# - Default cross-deps prefixes are build.sh's in-repo ones
#   (src-wk2/<arch>/prefix, produced by --build-deps).  Existing prefixes can
#   be reused, e.g. on the dev workstation:
#     --prefix-arm ~/webkit-hobbyos-wk2/arm/prefix
#     --prefix-x64 ~/webkit-hobbyos-wk2/intel/prefix
# - The x64 leg additionally requires the OS-side intel sysroot closure
#   (obj/intel/{crt0.o,libc.a,libcxx.a,setjmp.o} and obj/intel/icu/libicu*.a).
# - --from-existing: skip building and refresh the cache from the flat
#   already at build/<arch>/browser.bin (size + marker checks still run).
# - Local 'make disk.img' prefers build/<arch>/browser.bin or a fork
#   BROWSER_BIN over the cache, so a refreshed cache mainly serves machines
#   that have not built anything yet.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ARCHS="both"
JOBS=""
BUILD_DEPS=0
FROM_EXISTING=0
PREFIX_ARM=""
PREFIX_X64=""

usage() {
  echo "usage: $0 [--arch arm|x64|both] [-j N] [--build-deps]"
  echo "          [--prefix-arm DIR] [--prefix-x64 DIR] [--from-existing]"
  echo "  Rebuilds cache/browser-{arm,x64}.bin.xz from the vendored snapshot."
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCHS="$2"; shift 2;;
    arm|x64|intel|both) ARCHS="$1"; shift;;
    -j|--jobs) JOBS="$2"; shift 2;;
    --build-deps) BUILD_DEPS=1; shift;;
    --prefix-arm) PREFIX_ARM="$2"; shift 2;;
    --prefix-x64) PREFIX_X64="$2"; shift 2;;
    --from-existing) FROM_EXISTING=1; shift;;
    -h|--help) usage;;
    *) echo "unknown arg: $1" >&2; usage;;
  esac
done
case "$ARCHS" in
  both) ARCHS="arm x64";;
  intel) ARCHS="x64";;
  arm|x64) ;;
  *) echo "FATAL: --arch must be arm, x64 or both (got '$ARCHS')" >&2; exit 2;;
esac

log() { printf '[rebuild-browser] %s\n' "$*"; }

log "arch: $ARCHS — snapshot: $HERE"
FAILED=""

for A in $ARCHS; do
  if [ "$A" = arm ]; then BS=arm; NAME=arm; PREFIX="$PREFIX_ARM"; else BS=intel; NAME=x64; PREFIX="$PREFIX_X64"; fi
  FLAT="$HERE/build/$BS/browser.bin"
  CACHE="$HERE/cache/browser-$NAME.bin.xz"

  log "=== $A ($BS) ==="
  if [ "$FROM_EXISTING" = 1 ]; then
    if [ ! -s "$FLAT" ]; then log "FATAL: --from-existing but $FLAT is missing"; FAILED="$FAILED $A"; continue; fi
    log "using existing flat: $FLAT"
  else
    ARGS=(--arch "$BS")
    if [ -n "$PREFIX" ]; then ARGS+=(--prefix "$PREFIX"); fi
    if [ -n "$JOBS" ]; then ARGS+=(-j "$JOBS"); fi
    if [ "$BUILD_DEPS" = 1 ]; then ARGS+=(--build-deps); fi
    if ! bash "$HERE/build.sh" "${ARGS[@]}"; then
      log "build failed for $A — skipping (cache not refreshed)"
      FAILED="$FAILED $A"
      continue
    fi
    if [ ! -s "$FLAT" ]; then log "FATAL: expected flat missing after build: $FLAT"; FAILED="$FAILED $A"; continue; fi
  fi

  SZ=$(stat -c%s "$FLAT")
  if [ "$SZ" -lt 40000000 ]; then log "FATAL: $FLAT suspiciously small ($SZ B) — refusing to cache it"; FAILED="$FAILED $A"; continue; fi
  # Marker sanity.  NOTE: flats are raw objcopy images — symbol tables are not
  # included, so ELF symbol-name markers (e.g. WK5WindowDriver) never appear
  # here; check WK5 driver strings that live in .rodata instead.
  if ! grep -aqE 'WK5-THREAD-SMOKE|load-ok' "$FLAT"; then
    log "FATAL: no WK5 driver markers (load-ok / WK5-THREAD-SMOKE) in $FLAT — refusing to cache it"
    FAILED="$FAILED $A"; continue
  fi

  FLAT_SHA=$(sha256sum "$FLAT" | cut -d' ' -f1)
  OLD_SHA="(none)"
  if [ -f "$CACHE" ]; then OLD_SHA=$(sha256sum "$CACHE" | cut -d' ' -f1); fi

  log "re-compressing: $FLAT ($SZ B, sha256 $FLAT_SHA)"
  TMP="$CACHE.tmp.$$"
  if ! xz -T0 -6 -c "$FLAT" > "$TMP"; then log "FATAL: xz compression failed"; rm -f "$TMP"; FAILED="$FAILED $A"; continue; fi
  if ! xz -t "$TMP"; then log "FATAL: xz integrity check failed on fresh archive"; rm -f "$TMP"; FAILED="$FAILED $A"; continue; fi
  mv "$TMP" "$CACHE"
  CACHE_SHA=$(sha256sum "$CACHE" | cut -d' ' -f1)

  DEC_SHA=$(xz -dc "$CACHE" | sha256sum | cut -d' ' -f1)
  if [ "$DEC_SHA" != "$FLAT_SHA" ]; then
    log "FATAL: decompressed cache sha ($DEC_SHA) != flat sha ($FLAT_SHA)"; FAILED="$FAILED $A"; continue
  fi

  awk -v name="cache/browser-$NAME.bin.xz" -v sha="$CACHE_SHA" '
    { if ($2 == name) { print sha "  " $2; ok = 1 } else print }
    END { if (!ok) print sha "  " name }
  ' "$HERE/SHA256SUMS" > "$HERE/SHA256SUMS.tmp.$$"
  mv "$HERE/SHA256SUMS.tmp.$$" "$HERE/SHA256SUMS"

  log "cache updated: $CACHE ($(stat -c%s "$CACHE") B)"
  log "  sha256 $CACHE_SHA  (was $OLD_SHA)"
  log "  round-trip verified: decompressed cache sha256 == flat sha256"
done

if [ -n "$FAILED" ]; then
  log "FAILED:$FAILED"
  exit 1
fi
log "done.  Publish the refreshed cache with:"
log "  git add third_party/webkit-hobbyos/cache third_party/webkit-hobbyos/SHA256SUMS"
log "  git commit -m 'cache: rebuild browser binary cache from the vendored snapshot'"
