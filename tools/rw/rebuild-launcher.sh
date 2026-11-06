#!/usr/bin/env bash
# tools/rw/rebuild-launcher.sh — rebuild ONLY the two launcher artifacts the
# desktop harness stages (obj/rw-desktop/rwlaunch.bin and RimWorldLinux.ad9),
# without rebuilding the kernel or disk.img.
#
# Why: with RW_SKIP_BUILD=1 the harness re-uses the existing kernel *and* the
# already-built launcher binaries.  A launcher-only change (a new opt-in
# marker, an env var, a log row) then needs no full build, and the unit tier
# and desktop harness are never run concurrently in one worktree.
#
# The compile lines are the ones run-rimworld-desktop.sh uses; keep them in
# sync with it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
OUT="$ROOT/obj/rw-desktop"
G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
mkdir -p "$OUT"
[ -f "$LDIR/libc-2.31.so" ] || { echo "no glibc pin at $LDIR"; exit 2; }
[ -f "$DLIB/Scrt1.o" ] || { echo "no glibc dev pin at $DLIB"; exit 2; }

RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
clang "${UCFLAGS[@]}" -c src/user/rwlaunch.c -o "$OUT/rwlaunch.o" || exit 2
ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwlaunch.elf" "$OUT/rwlaunch.o" \
       obj/intel/libc.a || exit 2
llvm-objcopy -O binary "$OUT/rwlaunch.elf" "$OUT/rwlaunch.bin" || exit 2

SHADOW_CFLAGS=()
[ "${RW_NO_SHADOW:-0}" = 1 ] && SHADOW_CFLAGS=(-DRW_NO_SHADOW_PTHREAD)
gcc -O2 -Wall -Wextra -fPIE -pie -nostdlib -nostartfiles -rdynamic \
    "${SHADOW_CFLAGS[@]}" \
    -nostdinc -isystem "$D/usr/include" \
    -isystem "$D/usr/include/x86_64-linux-gnu" \
    -isystem "$(gcc -print-file-name=include)" \
    -o "$OUT/RimWorldLinux.ad9" src/user/rwmain.c \
    "$ROOT/tools/rw/mono-probe/condtrace.c" \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$LDIR/libdl-2.31.so" "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || exit 2

echo "rebuilt: $OUT/rwlaunch.bin ($(stat -c %s "$OUT/rwlaunch.bin") bytes), $OUT/RimWorldLinux.ad9 ($(stat -c %s "$OUT/RimWorldLinux.ad9") bytes)"
