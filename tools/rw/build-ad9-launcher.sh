#!/usr/bin/env bash
# tools/rw/build-ad9-launcher.sh -- rebuild ONLY the AD-9 PlayerMain launcher.
#
# The desktop harness (tools/rw/run-rimworld-desktop.sh) builds this as one
# step of its stage; when a lane is iterating on src/user/rwmain.c a full
# kernel/disk rebuild costs minutes per edit.  This produces the same binary
# at the same path (obj/rw-desktop/RimWorldLinux.ad9), so it can be paired
# with
#
#     RW_SKIP_BUILD=1 tools/rw/run-rimworld-desktop.sh
#
# which still stages /LIB64, /RWDSO, /RWMANAGED and installs this launcher as
# the game's executable.  Keep the two commands in one place: the flags below
# are the ones the harness uses (a glibc-2.31 PIE, -rdynamic, -nostdlib).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
export HOME=/home/sarah

G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
OUT="$ROOT/obj/rw-desktop"
mkdir -p "$OUT"

[ -f "$DLIB/Scrt1.o" ] || { echo "missing $DLIB/Scrt1.o (glibc-2.31 dev tree)"; exit 2; }

SHADOW_CFLAGS=()
if [ "${RW_NO_SHADOW:-0}" = 1 ]; then SHADOW_CFLAGS=(-DRW_NO_SHADOW_PTHREAD); fi

gcc -O2 -Wall -Wextra -fPIE -pie -nostdlib -nostartfiles -rdynamic \
    "${SHADOW_CFLAGS[@]}" \
    -nostdinc -isystem "$D/usr/include" \
    -isystem "$D/usr/include/x86_64-linux-gnu" \
    -isystem "$(gcc -print-file-name=include)" \
    -o "$OUT/RimWorldLinux.ad9" src/user/rwmain.c \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$LDIR/libdl-2.31.so" "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || {
  echo "AD-9 launcher link failed"; exit 2; }

readelf -h "$OUT/RimWorldLinux.ad9" | awk '/Type:/{print "  ad9:",$2}'
# NOTE: capture the symbol table first.  `readelf -sW ... | grep -q dlopen`
# under `set -o pipefail` reports the pipeline as failed whenever grep exits
# early and readelf takes SIGPIPE (141) -- measured: a correct launcher was
# rejected with "does not export dlopen".  One read, then match the text.
AD9_SYMS="$(readelf -sW "$OUT/RimWorldLinux.ad9")"
grep -qw dlopen <<<"$AD9_SYMS" || {
  echo "AD-9 launcher does not export dlopen"; exit 2; }
grep -qw sigaction <<<"$AD9_SYMS" || {
  echo "AD-9 launcher does not export sigaction"; exit 2; }
echo "  built: $OUT/RimWorldLinux.ad9"
