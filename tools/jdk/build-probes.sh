#!/usr/bin/env bash
# tools/jdk/build-probes.sh -- build the glibc-2.31 x86_64 probes in
# tools/jdk/*.c to obj/jdk/<name>.exe.
#
# Why not gcc: this host is macOS/arm64.  Bare `gcc` is Apple clang, which
# cannot emit an x86_64 SysV ELF, and the triple must be stated or clang targets
# Darwin and hands the objects to ld64.lld.  So: --target=x86_64-linux-gnu with
# -fuse-ld=lld, linking against the SAME glibc 2.31 pin the guest stages
# (a different sysroot would produce a binary whose GLIBC_* requirements the
# guest cannot satisfy -- the whole reason the pin exists).
#
# The link line mirrors tools/rw/build-ad9-launcher.sh (a glibc-2.31 PIE,
# -nostdlib -nostartfiles -rdynamic, explicit Scrt1/crti/crtn) with one
# difference that matters here: clang, not gcc, and `libpthread-2.31.so` named
# explicitly because the pin's libc.so.6 does not carry pthread_* under 2.31.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
G="${GLIBC231:-$HOME/.hermes/cache/scratch/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$G/usr/lib/x86_64-linux-gnu"
OUT="$ROOT/obj/jdk"
mkdir -p "$OUT"
[ -f "$LDIR/libc-2.31.so" ] || { echo "no glibc pin at $LDIR (set GLIBC231=)"; exit 2; }
[ -f "$DLIB/Scrt1.o" ] || { echo "no glibc dev pin at $DLIB"; exit 2; }

RC="$(clang -print-resource-dir)/include"
for src in "$@"; do :; done
if [ $# -eq 0 ]; then set -- "$ROOT"/tools/jdk/*.c; fi
rc=0
for src in "$@"; do
  name="$(basename "${src%.c}")"
  clang --target=x86_64-linux-gnu -fuse-ld=lld -O1 -g -Wall -fPIE -pie \
        -nostdlib -nostartfiles -rdynamic \
        -nostdinc -isystem "$G/usr/include" \
        -isystem "$G/usr/include/x86_64-linux-gnu" -isystem "$RC" \
        -o "$OUT/$name.exe" "$src" \
        "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
        "$LDIR/libpthread-2.31.so" "$LDIR/libm-2.31.so" "$LDIR/libc.so.6" \
        "$DLIB/crtn.o" \
        -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
        -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
    || { echo "FAILED: $src"; rc=2; continue; }
  echo "   built obj/jdk/$name.exe ($(stat -f%z "$OUT/$name.exe" 2>/dev/null || stat -c%s "$OUT/$name.exe") bytes)"
done
exit $rc
