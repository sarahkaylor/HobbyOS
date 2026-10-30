#!/usr/bin/env bash
# link-flat.sh -- link a static, flat HobbyOS user program (the repo's model:
# crt0.o + objects + archives + libc.a [+ libcxx.a], -T src/user/linker.ld,
# -e _start, then `llvm-objcopy -O binary` -> NAME.BIN).
#
#   third_party/x11/cross/link-flat.sh --arch arm|intel --out DIR/NAME.BIN \
#       [--prefix PREFIX] [--cxx] [--gc] -- <objects> <archives> [-L.. -l..]
#
# --prefix   X11 prefix for -L (default: third_party/x11/build/<arch>/prefix)
# --cxx      C++ closure (libcxx.a + libc.a; builds obj/<arch>/libcxx.a if absent)
# --gc       -Wl,--gc-sections (only for objects built -ffunction-sections)
# The intermediate ELF is kept next to the .BIN as NAME.elf (symbols for
# debugging / `nm`).  Inside build.sh use the hb_link_flat function instead
# (same thing, environment already sourced).
set -euo pipefail
X11="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH=""; OUT=""; PREFIX=""; CXXMODE=0; GC=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --prefix) PREFIX="$2"; shift 2;;
    --cxx) CXXMODE=1; shift;;
    --gc) GC=1; shift;;
    --) shift; break;;
    *) echo "link-flat: unknown arg $1" >&2; exit 2;;
  esac
done
[ -n "$ARCH" ] && [ -n "$OUT" ] || { echo "usage: $0 --arch arm|intel --out X.BIN [--prefix P] [--cxx] -- inputs..." >&2; exit 2; }
[ -n "$PREFIX" ] || PREFIX="$X11/build/$ARCH/prefix"
# shellcheck source=env.sh
. "$X11/cross/env.sh" "$ARCH" "$PREFIX"
for f in "$HB_OBJ/crt0.o" "$HB_OBJ/libc.a"; do
  [ -f "$f" ] || make -C "$HB_REPO" ARCH="$ARCH" "obj/$ARCH/libc.a" "obj/$ARCH/crt0.o" >/dev/null
done
if [ "$CXXMODE" = 1 ] && [ ! -f "$HB_OBJ/libcxx.a" ]; then
  make -C "$HB_REPO" ARCH="$ARCH" "obj/$ARCH/libcxx.a" >/dev/null
fi
mkdir -p "$(dirname "$OUT")"
elf="${OUT%.*}.elf"
drv="$CC"; [ "$CXXMODE" = 1 ] && drv="$CXX"
extra=(); [ "$GC" = 1 ] && extra+=(-Wl,--gc-sections)
"$drv" "${extra[@]}" -o "$elf" "$@" -L"$HB_PREFIX/lib"
llvm-objcopy -O binary "$elf" "$OUT"
echo "link-flat: $OUT ($(stat -c %s "$OUT") bytes)"
