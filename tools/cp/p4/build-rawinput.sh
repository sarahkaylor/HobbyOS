#!/usr/bin/env bash
# tools/cp/p4/build-rawinput.sh [OUT.exe] -- build src/user/cp/rawinput.c, the
# Gate P4 raw-input / mouselook / display-mode probe (lane C4-x2), as a
# Windows x86_64 console program that opens a window.
#
# Same recipe as tools/cp/build-winprobe.sh (clang -target x86_64-windows,
# Wine 11.0's own headers and PE import libs, winegcc as the link driver; no
# MinGW), plus user32 and gdi32.  Defaults to the X variant's build tree.
#
# Env: WINE_BUILD (default ~/cp/wine-build-x: src/wine-11.0 + build-x86_64)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WB="${WINE_BUILD:-$HOME/cp/wine-build-x}"
SRC="$WB/src/wine-11.0"
OBJ="$WB/build-x86_64"
OUT="${1:-$ROOT/obj/p4/rawinput.exe}"
mkdir -p "$(dirname "$OUT")"
[ -d "$SRC/include" ] && [ -x "$OBJ/tools/winegcc/winegcc" ] ||
  { echo "build-rawinput: no Wine build at $WB (CP_WINE_VARIANT=x tools/cp/build-wine.sh)" >&2; exit 2; }
O="${OUT%.exe}.o"
clang -c -o "$O" "$ROOT/src/user/cp/rawinput.c" \
  -I"$OBJ/include" -I"$SRC/include" -I"$SRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD \
  -target x86_64-windows -fuse-ld=lld --no-default-config -fno-strict-aliasing \
  -O1 -g -Wall -Wextra -Wno-unused-parameter
(cd "$OBJ" && tools/winegcc/winegcc -o "$OUT" --wine-objdir . -b x86_64-windows -mconsole \
  "$O" dlls/winecrt0/x86_64-windows/libwinecrt0.a \
  libs/compiler-rt/x86_64-windows/libcompiler-rt.a dlls/ucrtbase/x86_64-windows/libucrtbase.a \
  dlls/user32/x86_64-windows/libuser32.a dlls/gdi32/x86_64-windows/libgdi32.a \
  dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a \
  --no-default-config -Wl,-debug:dwarf)
file "$OUT" 2>/dev/null || true
ls -l "$OUT"
