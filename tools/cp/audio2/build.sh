#!/usr/bin/env bash
# tools/cp/audio2/build.sh -- the audio repro's guest programs (lane
# H5-audio2, docs/cp-audio2.md): a STATIC musl alsa-lib 1.2.14 (the pinned
# tarball tools/cp/audio/build-alsa.sh uses; nothing is downloaded) and two
# static-PIE musl ELFs that run as boot-wave members:
#   $WORK/ATONE.ELF   tools/cp/audio2/atone.c (winealsa-model player)
#   $WORK/ALOAD.ELF   tools/cp/audio2/aload.c (guest load generator)
# musl-gcc hides the host's /usr/include; alsa-lib needs <linux/*.h>, taken
# from the glibc-2.31 sysroot's linux-libc-dev 5.4 headers (copied into
# $WORK/kinc).  libasound.a carries its PCM plugins built in (plug,
# mmap_emul, lfloat, hw); the config is embedded in atone.c.
#
# Env: CP_ALSA_TARBALL (default ~/cp/lanes/audio-dl/alsa-lib-1.2.14.tar.bz2)
#      CP_AUDIO2_WORK  (default ~/cp/lanes/audio2-alsa)
#      GLIBC_SYSROOT   (default ~/cp/sysroot-glibc231; headers only)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TAR="${CP_ALSA_TARBALL:-$HOME/cp/lanes/audio-dl/alsa-lib-1.2.14.tar.bz2}"
WORK="${CP_AUDIO2_WORK:-$HOME/cp/lanes/audio2-alsa}"
SYS="${GLIBC_SYSROOT:-$HOME/cp/sysroot-glibc231}"
SHA=be9c88a0b3604367dd74167a2b754a35e142f670292ae47a2fdef27a2ee97a32
say() { printf 'audio2-build: %s\n' "$*"; }
die() { say "ERROR: $*"; exit 1; }
command -v musl-gcc >/dev/null || die "no musl-gcc"
[ -f "$TAR" ] || die "no $TAR (docs/cp-audio.md §5)"
[ "$(sha256sum "$TAR" | cut -d' ' -f1)" = "$SHA" ] || die "$TAR sha256 mismatch"
mkdir -p "$WORK/src"
SRC="$WORK/src/alsa-lib-1.2.14"
[ -f "$SRC/.ok" ] || { rm -rf "$SRC"; tar -C "$WORK/src" -xf "$TAR"; touch "$SRC/.ok"; }
K="$WORK/kinc"
if [ ! -f "$K/.ok" ]; then
  rm -rf "$K"; mkdir -p "$K"
  cp -r "$SYS/usr/include/linux" "$SYS/usr/include/asm-generic" "$K/"
  cp -r "$SYS/usr/include/x86_64-linux-gnu/asm" "$K/asm"
  touch "$K/.ok"
fi
DEST="$WORK/dest"
LIB="$DEST/CP/ALSA/lib/libasound.a"
if [ ! -f "$LIB" ]; then
  rm -rf "$WORK/build" "$DEST"; mkdir -p "$WORK/build"
  say "configure alsa-lib (musl-gcc, static)"
  ( cd "$WORK/build" && CC=musl-gcc CFLAGS="-O2 -g -fPIE -isystem $K" "$SRC/configure" \
      --host=x86_64-linux-musl --prefix=/CP/ALSA --enable-static --disable-shared \
      --disable-python --disable-topology --disable-ucm --disable-hwdep --disable-old-symbols \
      --disable-rawmidi --disable-seq --disable-mixer --without-debug ) > "$WORK/configure.log" 2>&1 ||
    { tail -20 "$WORK/configure.log"; die configure; }
  make -C "$WORK/build" ${MAKEFLAGS:+} -j16 > "$WORK/make.log" 2>&1 || { tail -20 "$WORK/make.log"; die make; }
  make -C "$WORK/build" install DESTDIR="$DEST" > "$WORK/install.log" 2>&1 || die install
fi
INC="$DEST/CP/ALSA/include"
musl-gcc -static-pie -fPIE -O2 -g -Wall -isystem "$K" -I"$INC" -o "$WORK/ATONE.ELF" \
  "$ROOT/tools/cp/audio2/atone.c" "$LIB" -lm -lpthread
musl-gcc -static-pie -fPIE -O2 -g -Wall -o "$WORK/ALOAD.ELF" "$ROOT/tools/cp/audio2/aload.c" -lpthread
say "ATONE.ELF $(stat -c %s "$WORK/ATONE.ELF") B, ALOAD.ELF $(stat -c %s "$WORK/ALOAD.ELF") B"
