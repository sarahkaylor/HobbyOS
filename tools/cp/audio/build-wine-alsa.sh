#!/usr/bin/env bash
# tools/cp/audio/build-wine-alsa.sh -- lane C4-audio's Wine 11.0 with
# winealsa.drv (docs/cp-audio.md §6), in its OWN work dir.
#
# It is the `x` build (~/cp/wine-build-x, read-only: its extracted source and
# its vkstub are used in place) with exactly ONE configure change:
#   --without-alsa dropped, ALSA_CFLAGS=-I$ALSA/include ALSA_LIBS="-L$ALSA/lib -lasound"
# where $ALSA is tools/cp/audio/build-alsa.sh's install (alsa-lib 1.2.14
# against the glibc 2.31 sysroot).  Every other flag, the compiler and the
# environment are copied from ~/cp/wine-build-x/build-x86_64/config.log.
# The stage then gets libasound.so.2* in /CP/WINE/lib and alsa's config in
# /CP/WINE/share/alsa (alsa-lib was configured with --prefix=/CP/WINE).
#
# tools/cp/build-wine.sh CP_WINE_ALSA=<prefix> is the same change as an opt-in
# of the canonical script (for the integrator's combined build); this script
# exists because build-wine.sh's pin step re-fetches Wine's GPG key (a
# download) and the lane works offline.
#
#   bash tools/cp/audio/build-wine-alsa.sh [configure|build|install|audit|all]
# Env: CP_AUDIO_WINE (default ~/cp/lanes/audio-wine), CP_ALSA_WORK (~/cp/lanes/audio-alsa),
#      WINE_X (~/cp/wine-build-x), CP_X11_WORK (~/cp/x11-build-x2),
#      CP_WINE_SYSROOT (~/cp/lanes/audio-sysroot), JOBS (16)
set -euo pipefail
WORK="${CP_AUDIO_WINE:-$HOME/cp/lanes/audio-wine}"
ALSA="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}/destdir/CP/WINE"
WX="${WINE_X:-$HOME/cp/wine-build-x}"
X11W="${CP_X11_WORK:-$HOME/cp/x11-build-x2}"
SYS="${CP_WINE_SYSROOT:-$HOME/cp/lanes/audio-sysroot}"
# A dangling libpthread.so / libdl.so makes ld fall back to the STATIC archive
# silently (measured 2026-11-17: ~/cp/sysroot-glibc231's dev links point into a
# deleted ~/cp/lanes/ext2b/obj/sysroot-glibc231; libasound came out without
# DT_NEEDED libdl/libpthread).  docs/cp-audio.md §5 has the repair recipe.
for _l in libpthread.so libdl.so librt.so libm.so; do
  [ -e "$SYS/usr/lib/x86_64-linux-gnu/$_l" ] || { printf 'ERROR: %s/usr/lib/x86_64-linux-gnu/%s dangles (sysroot broken)\n' "$SYS" "$_l"; exit 2; }
done
JOBS="${JOBS:-16}"
SRC="$WX/src/wine-11.0"
BLD="$WORK/build-x86_64"
STAGE="$WORK/stage"
PREFIX=/CP/WINE
X11DEV="$X11W/destdir/CP/X11"
STEP="${1:-all}"
say() { printf 'build-wine-alsa: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-1}"; }
[ -f "$SRC/configure" ] || die "no Wine source at $SRC" 2
[ -f "$ALSA/include/alsa/asoundlib.h" ] || die "no alsa-lib at $ALSA (tools/cp/audio/build-alsa.sh)" 2
mkdir -p "$WORK"

do_configure() {
  rm -rf "$BLD"; mkdir -p "$BLD"
  # The x build's recorded command line, --without-alsa removed.
  local orig
  orig=$(sed -n 's/^  \$ .*\/configure //p' "$WX/build-x86_64/config.log" | head -1)
  [ -n "$orig" ] || die "cannot read the x build's configure line"
  local args=() a
  for a in $orig; do [ "$a" = --without-alsa ] || args+=("$a"); done
  printf '%s\n' "${args[@]}" > "$WORK/configure-args.txt"
  say "configure: the x build's ${#args[@]} args minus --without-alsa; alsa from $ALSA"
  ( cd "$BLD" &&
    PKG_CONFIG_LIBDIR="$SYS/usr/lib/x86_64-linux-gnu/pkgconfig:$X11W/pc" PKG_CONFIG_PATH= \
    CC="clang --sysroot=$SYS" CFLAGS="-O2 -g" x86_64_CC=clang \
    LDFLAGS="-Wl,-rpath-link,$X11DEV/lib -L$WX/vkstub -L$ALSA/lib" \
    ALSA_CFLAGS="-I$ALSA/include" ALSA_LIBS="-L$ALSA/lib -lasound" \
    "$SRC/configure" "${args[@]}" ) > "$WORK/configure.log" 2>&1 ||
    { tail -30 "$WORK/configure.log"; die "configure failed"; }
  grep -qE '^ALSA_LIBS = .*-lasound' "$BLD/Makefile" || die "configure dropped ALSA_LIBS (snd_pcm_hw_params_get_access_mask?)"
  say "configure: $(grep -E '^ALSA_LIBS = ' "$BLD/Makefile")"
  diff <(grep -E '^#(define|undef)' "$WX/build-x86_64/include/config.h") \
       <(grep -E '^#(define|undef)' "$BLD/include/config.h") > "$WORK/config-h.diff" || true
  say "configure: config.h vs the x build: $(grep -c '^[<>]' "$WORK/config-h.diff") changed lines"
  sed 's/^/  /' "$WORK/config-h.diff"
}

do_build() {
  [ -f "$BLD/Makefile" ] || do_configure
  local t0=$SECONDS
  make -C "$BLD" -j"$JOBS" > "$WORK/build.log" 2>&1 ||
    { grep -nE 'error:|Error [0-9]' "$WORK/build.log" | head -30; die "make failed"; }
  say "build: done in $((SECONDS - t0)) s"
}

do_install() {
  rm -rf "$STAGE"
  make -C "$BLD" install-lib DESTDIR="$STAGE" > "$WORK/install.log" 2>&1 || die "install-lib failed"
  rm -rf "$STAGE$PREFIX/share/man" "$STAGE$PREFIX/share/applications"
  # build-wine.sh's strip (CP_WINE_STRIP=1 default)
  find "$STAGE$PREFIX/lib/wine/x86_64-windows" -type f \( -name '*.dll' -o -name '*.exe' \
       -o -name '*.drv' -o -name '*.sys' -o -name '*.ocx' -o -name '*.cpl' -o -name '*.acm' \
       -o -name '*.ax' -o -name '*.tlb' -o -name '*.ds' -o -name '*.msstyles' -o -name '*.com' \) \
       -exec llvm-strip --strip-debug {} + 2>/dev/null || true
  find "$STAGE$PREFIX" -type f \( -name '*.so' -o -perm -u+x \) -exec sh -c \
    'for f; do [ "$(head -c4 "$f" | od -An -c | tr -d " ")" = 177ELF ] && strip --strip-debug "$f"; done' sh {} +
  cp -a "$ALSA"/lib/libasound.so.2* "$STAGE$PREFIX/lib/"
  cp -a "$ALSA/share/alsa" "$STAGE$PREFIX/share/"
  [ -f "$STAGE$PREFIX/lib/wine/x86_64-unix/winealsa.so" ] || die "no winealsa.so staged"
  say "install: $(du -sh "$STAGE$PREFIX" | cut -f1) at $STAGE$PREFIX; winealsa.so needs: $(readelf -d "$STAGE$PREFIX/lib/wine/x86_64-unix/winealsa.so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
  ( cd "$STAGE$PREFIX" && find . -type f | LC_ALL=C sort | xargs sha256sum ) > "$WORK/stage-manifest.sha256"
  # What differs from the x stage (the integrator's merge list).
  diff <(cut -c67- "$WX/stage-manifest.sha256" 2>/dev/null | sort) <(cut -c67- "$WORK/stage-manifest.sha256" | sort) \
    > "$WORK/stage-files.diff" || true
  join -j2 <(awk '{print $1, $2}' "$WX/stage-manifest.sha256" | sort -k2) \
           <(awk '{print $1, $2}' "$WORK/stage-manifest.sha256" | sort -k2) |
    awk '$2 != $3 {print "changed", $1}' > "$WORK/stage-changed.txt"
  say "install: vs the x stage: $(grep -c '^>' "$WORK/stage-files.diff") added, $(grep -c '^<' "$WORK/stage-files.diff") removed, $(wc -l < "$WORK/stage-changed.txt") changed files"
}

do_audit() {
  local bad=0 f v
  while IFS= read -r f; do
    [ "$(head -c4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] || continue
    v=$(objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
    if [ -n "$v" ] && [ "$(printf '%s\n2.31\n' "$v" | sort -V | tail -1)" != 2.31 ]; then
      say "AUDIT FAIL: ${f#"$STAGE"} needs GLIBC_$v"; bad=1
    fi
  done < <(find "$STAGE$PREFIX" -type f \( -perm -u+x -o -name '*.so' -o -name '*.so.*' \))
  [ "$bad" = 0 ] || die "glibc audit failed"
  say "audit: every ELF in the stage needs <= GLIBC_2.31"
}

case "$STEP" in
  configure) do_configure ;;
  build) do_build ;;
  install) do_install ;;
  audit) do_audit ;;
  all) do_configure; do_build; do_install; do_audit ;;
  *) die "unknown step $STEP" 2 ;;
esac
