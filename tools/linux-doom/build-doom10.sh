#!/usr/bin/env bash
# build-doom10.sh — build id Software's linuxdoom-1.10 as a static-PIE
# x86_64 Linux ELF (musl), the binary the HobbyOS compat track runs.
#
#   ./build-doom10.sh                 # -> $BUILD/out/doom10.elf
#   DOOM10_SRC=/path/to/linuxdoom-1.10 ./build-doom10.sh
#
# One command, from a pristine checkout of github.com/id-software/doom:
# the script copies linuxdoom-1.10 to $BUILD/doom10-src, applies the LP64 /
# modern-toolchain patch, adds two header shims, compiles every .c with
# musl-gcc and links -static-pie against the musl X11 sysroot built by
# build-x11-musl-sysroot.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${HOBBYOS_DOOM_BUILD:-$HOME/hobbyos-doom/build}"
CACHE="${HOBBYOS_DOOM_CACHE:-$HOME/.cache/hobbyos-doom}"
SRC="${DOOM10_SRC:-$HOME/hobbyos-doom/doom-src/linuxdoom-1.10}"
WORK="$BUILD/doom10-src"
OUT="${OUT:-$BUILD/out}"
CC="${CC:-musl-gcc}"

[[ -f "$SRC/d_main.c" ]] || { echo "ERROR: linuxdoom-1.10 sources not found at $SRC" >&2
  echo "       set DOOM10_SRC=<checkout>/linuxdoom-1.10" >&2; exit 1; }

# --- static X11 for musl ----------------------------------------------------
if [[ ! -f "$CACHE/x11-musl.env" ]]; then
  echo "[doom10] building the musl X11 sysroot (first run)"
  "$HERE/build-x11-musl-sysroot.sh"
fi
# shellcheck disable=SC1090
source "$CACHE/x11-musl.env"

# --- pristine copy + patches ------------------------------------------------
rm -rf "$WORK"; mkdir -p "$WORK"
cp -a "$SRC/." "$WORK/"
( cd "$WORK" && patch -p1 --no-backup-if-mismatch < "$HERE/patches/doom10-lp64-toolchain.patch" )

mkdir -p "$WORK/shim/linux"
cp "$HERE/shim/errnos.h" "$WORK/shim/errnos.h"
cp "$HERE/shim/linux/soundcard.h" "$WORK/shim/linux/soundcard.h"

# --- compile ----------------------------------------------------------------
# Flags, and why each one is not optional:
#   -std=gnu89   GCC 15 defaults to C23, where `typedef enum {false,true} boolean`
#                (doomdef.h) is a hard error.  Doom 1.10 is K&R-era C.
#   -fcommon     tentative definitions in headers (doomstat.h) need common symbols.
#   -DIPPORT_USERRESERVED=5000
#                BSD-ism musl's <netinet/in.h> does not define; i_net.c uses it
#                for DOOMPORT (5000+0x1d = 5029).
#   -Ishim       <errnos.h> is a typo for <errno.h> in i_video.c; <linux/soundcard.h>
#                is compiled out under SNDSERV but still #included by i_sound.c.
#   -fno-strict-aliasing
#                Doom 1.10 type-puns constantly (`*(int*)&fixed`, casting a
#                byte buffer to structs).  GCC's aliasing assumptions are not
#                valid for it; every port of this codebase disables them.
CFLAGS=(-std=gnu89 -fcommon -O2 -fno-strict-aliasing -DNORMALUNIX -DLINUX
        -DIPPORT_USERRESERVED=5000 -Ishim $X11_MUSL_CFLAGS)
[[ "${DEBUG:-0}" == 1 ]] && CFLAGS+=(-g -O0)
# shellcheck disable=SC2206
[[ -n "${EXTRA_CFLAGS:-}" ]] && CFLAGS+=(${EXTRA_CFLAGS})

echo "[doom10] compiling $(ls "$WORK"/*.c | wc -l) translation units with $CC"
OBJ="$WORK/obj"; mkdir -p "$OBJ"
for f in "$WORK"/*.c; do
  $CC "${CFLAGS[@]}" -c "$f" -o "$OBJ/$(basename "${f%.c}").o" &
done
wait
n_c=$(ls "$WORK"/*.c | wc -l); n_o=$(ls "$OBJ"/*.o 2>/dev/null | wc -l)
[[ "$n_c" == "$n_o" ]] || { echo "[doom10] ERROR: $((n_c-n_o)) translation unit(s) failed" >&2; exit 1; }

mkdir -p "$OUT"
echo "[doom10] linking $OUT/doom10.elf"
$CC -static-pie "${CFLAGS[@]}" -o "$OUT/doom10.elf" "$OBJ"/*.o $X11_MUSL_LIBS -lm

# Alpine's static archives carry .debug_info, which balloons the ELF to ~7.5 MB.
# The deliverable ships on a FAT image, so strip by default (1.5 MB); STRIP=0
# keeps the symbols.
if [[ "${STRIP:-1}" == 1 ]]; then strip -s "$OUT/doom10.elf"; fi
readelf -d "$OUT/doom10.elf" | grep -q NEEDED && { echo "[doom10] ERROR: dynamic deps" >&2; exit 1; }

# --- report -----------------------------------------------------------------
echo
echo "=== doom10.elf ==="
file "$OUT/doom10.elf"
readelf -h "$OUT/doom10.elf" | grep -E 'Class|Machine|Type'
ls -l "$OUT/doom10.elf"
sha256sum "$OUT/doom10.elf"
