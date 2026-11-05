#!/usr/bin/env bash
# build_sdl_pie.sh -- produce obj/sdl_test_app_pie, the x86_64 musl static-PIE
# the GB-013 loader maps as SDLAPP.BIN.
#
# Why this is a script and not a one-line recipe (GB-137): the fixture must be
# a REAL musl static-PIE.  A glibc static-PIE produced by
# `clang --target=x86_64-linux-musl` walks DT_RELA at link-time addresses and
# faults at VA=0xCF8 in the v2 address space, so a missing musl compiler must
# fail loudly instead of shipping something else.
#
# Linux has musl-gcc (`apt install musl-tools`) and always builds from source.
# macOS has none, and no Homebrew musl cross toolchain can substitute: they all
# ship a static-only (non-PIC) libc.a, so `-static-pie` dies with
#   relocation R_X86_64_32 against `.rodata.__init_libc.str1.1' can not be used
#   when making a PIE object
# (measured on messense/macos-cross-toolchains and FiloSottile/musl-cross), and
# `zig cc -target x86_64-linux-musl` emits PT_TLS + R_X86_64_TPOFF64, which the
# loader refuses by name.  On such a host this script stages the committed
# cache -- the musl static-PIE built on Linux by this same recipe -- the same
# pattern as third_party/webkit-hobbyos/cache/browser-*.bin.xz (a host-limited
# artifact resolved by browser-staged).
#
# Usage:
#   build_sdl_pie.sh <out> <musl-cc-or-empty>     # build, or stage the cache
#   build_sdl_pie.sh --record                     # refresh the cache manifest
#
# Staleness is CONTENT-addressed, not mtime-based: the cache ships a .deps file
# with the sha256 of every source it was built from, and the stage path refuses
# a cache whose recorded sources no longer match.  (A fresh clone gives every
# file the same checkout mtime, so an mtime rule would misfire on every clone.)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

CACHE="third_party/sdl/cache/sdlapp-x86_64-musl.elf"
CACHE_SHA="$CACHE.sha256"
CACHE_DEPS="$CACHE.deps"

# The fixture's build inputs -- the same list the Makefile rule declares.
SDL_PIE_SRCS=(
  tools/sdl/sdl_test_app.c
  src/include/fb_pixel.h
  src/include/linux_input.h
  src/include/display_mode.h
)

log() { printf '[sdl] %s\n' "$*"; }
die() { printf '[sdl] ERROR: %s\n' "$*" >&2; exit 1; }

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

# "<sha>  <path>" for every build input.  The list is fixed and ordered, so a
# changed file or a changed list both change the manifest text.
deps_manifest() {
  local f
  for f in "${SDL_PIE_SRCS[@]}"; do
    [ -f "$f" ] || die "missing source $f"
    printf '%s  %s\n' "$(sha256_of "$f")" "$f"
  done
}

# e_type: 3 == ET_DYN (static-PIE); 2 == ET_EXEC means a fixed-address binary
# the loader rejects below the user window.
check_et_dyn() {
  [ -f "$1" ] || die "$1 was not produced"
  local magic type
  magic="$(od -An -tx1 -j0 -N4 "$1" | tr -d ' \n')"
  [ "$magic" = "7f454c46" ] || die "$1 is not an ELF image (magic '$magic')"
  type="$(od -An -tx1 -j16 -N2 "$1" | tr -d ' \n')"
  [ "$type" = "0300" ] || die "$1 is not an ET_DYN static-PIE (e_type='$type'); refusing to stage it"
}

# --- record mode: refresh the cache manifest after replacing the cache ------
if [ "${1:-}" = "--record" ]; then
  [ -f "$CACHE" ] || die "no cache at $CACHE to record"
  deps_manifest > "$CACHE_DEPS"
  sha256_of "$CACHE" > "$CACHE_SHA"
  log "recorded $CACHE (sha256 $(cat "$CACHE_SHA")); sources:"
  sed 's/^/       /' "$CACHE_DEPS"
  exit 0
fi

OUT="${1:?usage: build_sdl_pie.sh <out> <musl-cc-or-empty> | --record}"
MUSL_CC="${2:-}"

# --- a real musl-gcc exists (Linux): build from source ---------------------
if [ -n "$MUSL_CC" ]; then
  log "$MUSL_CC -> $OUT (from source)"
  $MUSL_CC -fPIE -static-pie -I"$REPO_ROOT/src/include" tools/sdl/sdl_test_app.c -o "$OUT"
  check_et_dyn "$OUT"
  exit 0
fi

# --- no musl compiler on this host: stage the committed cache --------------
[ -f "$CACHE" ] || die "no musl C compiler (MUSL_CC empty) and no prebuilt cache at $CACHE
  Linux:  sudo apt install musl-tools        (provides musl-gcc)
  macOS:  no Homebrew musl toolchain can link -static-pie; build on Linux or
          refresh the cache per third_party/sdl/README.md"
[ -f "$CACHE_SHA" ]  || die "$CACHE has no .sha256; refusing an unverified binary"
[ -f "$CACHE_DEPS" ] || die "$CACHE has no .deps; refusing a cache with unknown sources"

want="$(tr -d '[:space:]' < "$CACHE_SHA")"
got="$(sha256_of "$CACHE")"
[ "$want" = "$got" ] || die "$CACHE sha256 mismatch (want $want, got $got)"

cur="$(deps_manifest)"
rec="$(cat "$CACHE_DEPS")"
if [ "$cur" != "$rec" ]; then
  die "the prebuilt SDLAPP.BIN is stale -- its sources changed since the cache was made.
  $CACHE.deps no longer matches $(echo "${SDL_PIE_SRCS[*]}")
  Rebuild it on a host with musl-gcc (third_party/sdl/README.md); do not stage it."
fi

mkdir -p "$(dirname "$OUT")"
cp "$CACHE" "$OUT"
chmod +x "$OUT"
check_et_dyn "$OUT"
log "prebuilt musl static-PIE (no musl compiler on this host) -> $OUT"
