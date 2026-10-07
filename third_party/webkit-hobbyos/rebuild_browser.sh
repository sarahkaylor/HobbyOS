#!/usr/bin/env bash
# rebuild_browser.sh — regenerate the optional binary cache
# (cache/browser-{arm,x64}.bin.xz) from the vendored WebKit snapshot in this
# directory.  Source-first: the cache is a convenience artifact that this
# script rebuilds from third_party source; it is never a requirement.
#
#   bash third_party/webkit-hobbyos/rebuild_browser.sh [--arch arm|x64|both] [-j N]
#        [--prefix-arm DIR] [--prefix-x64 DIR] [--from-existing]
#        [--no-build-deps] [--no-build-closure]
#
# For each requested architecture:
#   0. ensures the OS-side sysroot closure (obj/<arch>/{crt0.o,libc.a,libcxx.a
#      [,setjmp.o]}, obj/<arch>/icu/libicuuc.a) — missing pieces are built from
#      the OS tree's own make targets (libs first, then ICU, whose link probe
#      needs them).  --no-build-closure reports instead of building.
#   0b. ensures the cross-deps staging prefix; when incomplete, build.sh
#      --build-deps builds it from the committed third_party tarballs.
#      --no-build-deps reports instead.
#   1. runs build.sh (extract -> configure -> ninja -> flat
#      build/<arch>/browser.bin, with marker + sha checks)
#   2. re-compresses the flat with the committed settings (xz -T0 -6, CRC64)
#      into cache/browser-<name>.bin.xz  (arm -> browser-arm, x64 -> browser-x64;
#      build.sh's "intel" arch is mapped internally)
#   3. rewrites the cache lines in SHA256SUMS
#   4. prints the new flat/cache hashes; commit cache/ + SHA256SUMS to publish
#
# Notes:
# - A first run on a fresh clone is LONG: closure + cross-deps + a full WebKit
#   build per arch (hours for both on a 16-core machine).  -j N is forwarded
#   to the closure make calls, the deps build and ninja.
# - Existing prefixes can be reused instead of building deps, e.g. on the dev
#   workstation:
#     --prefix-arm ~/webkit-hobbyos-wk2/arm/prefix
#     --prefix-x64 ~/webkit-hobbyos-wk2/intel/prefix
# - --from-existing: skip stages 0/0b/1 and refresh the cache from the flat
#   already at build/<arch>/browser.bin (size + marker checks still run).
# - Local 'make disk.img' prefers build/<arch>/browser.bin or a fork
#   BROWSER_BIN over the cache, so a refreshed cache mainly serves machines
#   that have not built anything yet.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

ARCHS="both"
JOBS=""
BUILD_DEPS=1
BUILD_CLOSURE=1
FROM_EXISTING=0
PREFIX_ARM=""
PREFIX_X64=""

usage() {
  echo "usage: $0 [--arch arm|x64|both] [-j N]"
  echo "          [--prefix-arm DIR] [--prefix-x64 DIR] [--from-existing]"
  echo "          [--no-build-deps] [--no-build-closure]"
  echo "  Rebuilds cache/browser-{arm,x64}.bin.xz from the vendored snapshot."
  echo "  Missing cross-deps and the OS-side sysroot closure are built"
  echo "  automatically; --no-build-* turns that off."
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCHS="$2"; shift 2;;
    arm|x64|intel|both) ARCHS="$1"; shift;;
    -j|--jobs) JOBS="$2"; shift 2;;
    --build-deps) BUILD_DEPS=1; shift;;
    --no-build-deps) BUILD_DEPS=0; shift;;
    --no-build-closure) BUILD_CLOSURE=0; shift;;
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

# Keep the lib list in sync with build.sh's CHECK_LIBS (its stage-2 check).
deps_missing() {   # $1 = prefix; prints the missing lib names ("" = complete)
  local p="$1" miss="" l
  local libs="libz.a libpng.a libjpeg.a libwebp.a libwebpdemux.a libwebpmux.a libfreetype.a libharfbuzz.a libsqlite3.a libxml2.a libcurl.a libmbedtls.a"
  for l in $libs; do
    if [ ! -f "$p/lib/$l" ]; then miss="$miss $l"; fi
  done
  printf '%s' "$miss"
}

# Keep the required pieces in sync with build.sh's stage-3 sysroot check.
closure_missing() {  # $1 = build.sh arch; prints the missing pieces ("" = complete)
  local bs="$1" obj="$ROOT/obj/$1" miss="" o
  local need="crt0.o libc.a libcxx.a"
  if [ "$bs" = intel ]; then need="$need setjmp.o"; fi
  for o in $need; do
    if [ ! -f "$obj/$o" ]; then miss="$miss $o"; fi
  done
  if [ ! -f "$obj/icu/libicuuc.a" ]; then miss="$miss icu/libicuuc.a"; fi
  printf '%s' "$miss"
}

ensure_closure() {   # $1 = build.sh arch; returns 0 when the closure is present
  local bs="$1" obj="$ROOT/obj/$1" o miss mk mj=""
  local need="crt0.o libc.a libcxx.a"
  if [ "$bs" = intel ]; then need="$need setjmp.o"; fi
  miss="$(closure_missing "$bs")"
  if [ -z "$miss" ]; then
    log "sysroot closure OK: $obj"
    return 0
  fi
  if [ "$BUILD_CLOSURE" != 1 ]; then
    local hint="obj/$bs/crt0.o obj/$bs/libc.a obj/$bs/libcxx.a"
    if [ "$bs" = intel ]; then hint="$hint obj/$bs/setjmp.o"; fi
    hint="$hint obj/$bs/icu/libicuuc.a"
    log "FATAL: sysroot closure incomplete at $obj:$miss"
    log "  build it (OS tree):  make -C $ROOT ARCH=$bs $hint"
    return 1
  fi
  log "sysroot closure incomplete:$miss — building missing pieces from the OS tree"
  if [ -n "$JOBS" ]; then mj="-j$JOBS"; fi
  mk=""
  for o in $need; do
    if [ ! -f "$obj/$o" ]; then mk="$mk obj/$bs/$o"; fi
  done
  if [ -n "$mk" ]; then
    log "  make -C $ROOT ARCH=$bs $mj$mk"
    if ! make -C "$ROOT" ARCH="$bs" $mj $mk; then
      log "FATAL: closure build failed ($mk)"
      return 1
    fi
  fi
  if [ ! -f "$obj/icu/libicuuc.a" ]; then
    log "  make -C $ROOT ARCH=$bs $mj obj/$bs/icu/libicuuc.a   (ICU cross build: host stage + target — long on first run)"
    if ! make -C "$ROOT" ARCH="$bs" $mj "obj/$bs/icu/libicuuc.a"; then
      log "FATAL: ICU closure build failed"
      return 1
    fi
  fi
  miss="$(closure_missing "$bs")"
  if [ -n "$miss" ]; then
    log "FATAL: sysroot closure still incomplete after build:$miss"
    return 1
  fi
  log "sysroot closure OK: $obj (built)"
  return 0
}

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
    if [ -n "$PREFIX" ]; then EP="$PREFIX"; else EP="$HERE/src-wk2/$BS/prefix"; fi
    if ! ensure_closure "$BS"; then FAILED="$FAILED $A"; continue; fi
    DMISS="$(deps_missing "$EP")"
    ARGS=(--arch "$BS")
    if [ -n "$PREFIX" ]; then ARGS+=(--prefix "$PREFIX"); fi
    if [ -n "$JOBS" ]; then ARGS+=(-j "$JOBS"); fi
    if [ -n "$DMISS" ]; then
      if [ "$BUILD_DEPS" = 1 ]; then
        log "cross-deps prefix incomplete: $EP"
        log "  missing:$DMISS"
        log "  -> build.sh --build-deps will build them from the committed third_party tarballs (long on first run)"
        ARGS+=(--build-deps)
      else
        log "FATAL: cross-deps prefix incomplete: $EP — missing:$DMISS"
        log "  (re-run without --no-build-deps, or point --prefix-<arch> at an existing prefix)"
        FAILED="$FAILED $A"
        continue
      fi
    else
      log "cross-deps prefix OK: $EP"
    fi
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
