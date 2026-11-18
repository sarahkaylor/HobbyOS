#!/usr/bin/env bash
# tools/cp/audio/build-alsa.sh -- alsa-lib for the HobbyOS compat class (lane
# C4-audio, docs/cp-audio.md §5): built with clang against the pinned glibc
# 2.31 sysroot (never host headers), installed under the Wine prefix so it
# rides in the Wine stage:
#
#   $WORK/destdir/CP/WINE/lib/libasound.so.2*     (LD_LIBRARY_PATH has /CP/WINE/lib)
#   $WORK/destdir/CP/WINE/include/alsa/            (Wine's configure: ALSA_CFLAGS)
#   $WORK/destdir/CP/WINE/share/alsa/              (alsa.conf + pcm/*.conf, the
#                                                   compiled-in config dir)
#   $WORK/alsatone                                 (tools/cp/audio/alsatone.c)
#
# alsa.conf gets a block appended (the only change to upstream config):
# `plughw` and `default` become plug over mmap_emul over hw (card 0, the
# virtio-snd card), and ctl default is card 0.  alsa-lib's cards/ directory
# has no entry for the "virtio-snd" driver name, and the guest has no
# /etc/asound.conf or ~/.asoundrc.
#
# Env: CP_ALSA_TARBALL (default ~/cp/lanes/audio-dl/alsa-lib-1.2.14.tar.bz2)
#      CP_ALSA_WORK    (default ~/cp/lanes/audio-alsa)
#      CP_WINE_SYSROOT (default ~/cp/lanes/audio-sysroot: a copy of
#                      ~/cp/sysroot-glibc231 with its dangling dev links re-pointed)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TAR="${CP_ALSA_TARBALL:-$HOME/cp/lanes/audio-dl/alsa-lib-1.2.14.tar.bz2}"
WORK="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}"
SYS="${CP_WINE_SYSROOT:-$HOME/cp/lanes/audio-sysroot}"
# A dangling libpthread.so / libdl.so makes ld fall back to the STATIC archive
# silently (measured 2026-11-17: ~/cp/sysroot-glibc231's dev links point into a
# deleted ~/cp/lanes/ext2b/obj/sysroot-glibc231; libasound came out without
# DT_NEEDED libdl/libpthread).  docs/cp-audio.md §5 has the repair recipe.
for _l in libpthread.so libdl.so librt.so libm.so; do
  [ -e "$SYS/usr/lib/x86_64-linux-gnu/$_l" ] || { printf 'ERROR: %s/usr/lib/x86_64-linux-gnu/%s dangles (sysroot broken)\n' "$SYS" "$_l"; exit 2; }
done
PREFIX=/CP/WINE
DEST="$WORK/destdir"
say() { printf 'build-alsa: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-1}"; }
[ -f "$TAR" ] || die "no tarball $TAR (ask the integrator; docs/cp-audio.md §5)" 2
[ -f "$SYS/usr/include/stdio.h" ] || die "no sysroot at $SYS" 2
say "tarball $TAR sha256 $(sha256sum "$TAR" | cut -d' ' -f1) size $(stat -c %s "$TAR")"
mkdir -p "$WORK/src"
NAME=$(basename "$TAR"); NAME=${NAME%.tar.*}
SRC="$WORK/src/$NAME"
if [ ! -f "$SRC/.ok" ]; then
  rm -rf "$SRC"
  tar -C "$WORK/src" -xf "$TAR"
  touch "$SRC/.ok"
fi
BLD="$WORK/build"
rm -rf "$BLD" "$DEST"; mkdir -p "$BLD"
say "configure $NAME (clang --sysroot=$SYS, prefix $PREFIX)"
( cd "$BLD" && CC="clang --sysroot=$SYS" CFLAGS="-O2 -g" \
  "$SRC/configure" --host=x86_64-linux-gnu --prefix="$PREFIX" \
    --enable-shared --disable-static --disable-python --disable-topology --disable-ucm \
    --disable-alisp --disable-hwdep --disable-old-symbols \
    --without-debug ) > "$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log"; die "configure"; }
say "make"
make -C "$BLD" > "$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; die "make"; }
make -C "$BLD" install DESTDIR="$DEST" > "$WORK/install.log" 2>&1 || { tail -30 "$WORK/install.log"; die "install"; }
CONF="$DEST$PREFIX/share/alsa/alsa.conf"
[ -f "$CONF" ] || die "no $CONF after install"
cat >> "$CONF" <<'EOF'

# HobbyOS (tools/cp/audio/build-alsa.sh, docs/cp-audio.md §5): card 0 is the
# kernel's virtio-snd card, which offers RW access only (no data mmap).
# alsa-lib's converters (lfloat, rate, route) need an mmap-capable slave and
# plug does not insert mmap_emul for a RW-only slave behind a converter
# (measured: FLOAT_LE through plughw -> EINVAL), so plughw and default are plug
# over mmap_emul over hw.
pcm.!plughw {
	@args [ CARD DEV SUBDEV ]
	@args.CARD { type string default { @func getenv vars [ ALSA_PCM_CARD ALSA_CARD ] default { @func refer name defaults.pcm.card } } }
	@args.DEV { type integer default { @func igetenv vars [ ALSA_PCM_DEVICE ] default { @func refer name defaults.pcm.device } } }
	@args.SUBDEV { type integer default -1 }
	type plug
	slave.pcm { type mmap_emul slave.pcm { type hw card $CARD device $DEV subdevice $SUBDEV } }
}
pcm.!default { type plug slave.pcm { type mmap_emul slave.pcm "hw:0,0" } }
ctl.!default { type hw card 0 }
EOF
LIB=$(readlink -f "$DEST$PREFIX/lib/libasound.so.2")
say "installed $(basename "$LIB") $(stat -c %s "$LIB") B"
# Audit: nothing newer than GLIBC_2.31.
bad=$(objdump -T "$LIB" | grep -o 'GLIBC_[0-9.]*' | sort -uV | awk -F_ '{split($2,v,"."); if (v[1]>2 || (v[1]==2 && v[2]>31)) print}')
[ -z "$bad" ] || die "libasound needs $bad (> GLIBC_2.31)"
readelf -d "$LIB" | grep -q 'Shared library: \[libdl.so.2\]' || die "libasound has no DT_NEEDED libdl.so.2 (static libdl.a linked in?)"
say "audit: glibc versions used: $(objdump -T "$LIB" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tr '\n' ' ')"
clang --sysroot="$SYS" -O2 -Wall -I"$DEST$PREFIX/include" -o "$WORK/alsatone" "$ROOT/tools/cp/audio/alsatone.c" \
  -L"$DEST$PREFIX/lib" -lasound -lm
say "alsatone: $WORK/alsatone needs $(readelf -d "$WORK/alsatone" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
say "done: $DEST$PREFIX"
