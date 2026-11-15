#!/usr/bin/env bash
# tools/cp/build-winprobe.sh [OUT.exe] -- build src/user/cp/winprobe.c as a
# Windows x86_64 console program (lane C2-wine2, Gate P2).
#
# No MinGW and no download: clang 21 targets x86_64-windows (the MSVC ABI, so
# __try/__except is native SEH) and links with lld against the PE import
# libraries and winecrt0 that the Wine 11.0 build (tools/cp/build-wine.sh)
# already produced.  The headers are Wine's own Windows headers.  The result is
# an ordinary (non-builtin) PE that Wine loads as a native exe.
#
# Env: WINE_BUILD (default ~/cp/wine-build: src/wine-11.0 + build-x86_64)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WB="${WINE_BUILD:-$HOME/cp/wine-build}"
SRC="$WB/src/wine-11.0"
OBJ="$WB/build-x86_64"
OUT="${1:-$ROOT/obj/winprobe/winprobe.exe}"
mkdir -p "$(dirname "$OUT")"
[ -d "$SRC/include" ] && [ -x "$OBJ/tools/winegcc/winegcc" ] ||
  { echo "build-winprobe: no Wine build at $WB (run tools/cp/build-wine.sh)" >&2; exit 2; }
O="${OUT%.exe}.o"
clang -c -o "$O" "$ROOT/src/user/cp/winprobe.c" \
  -I"$OBJ/include" -I"$SRC/include" -I"$SRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD \
  -target x86_64-windows -fuse-ld=lld --no-default-config -fno-strict-aliasing \
  -fasync-exceptions -O1 -g -Wall -Wextra -Wno-unused-parameter
# Same driver and library order Wine links its own console programs with
# (programs/hostname in the build log), minus --wine-builtin.
(cd "$OBJ" && tools/winegcc/winegcc -o "$OUT" --wine-objdir . -b x86_64-windows -mconsole \
  "$O" dlls/winecrt0/x86_64-windows/libwinecrt0.a \
  libs/compiler-rt/x86_64-windows/libcompiler-rt.a dlls/ucrtbase/x86_64-windows/libucrtbase.a \
  dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a \
  --no-default-config -Wl,-debug:dwarf)
file "$OUT" 2>/dev/null || true
ls -l "$OUT"
