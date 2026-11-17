#!/usr/bin/env bash
# tools/cp/k5/build-thrstorm.sh [OUT.exe] -- build src/user/cp/thrstorm.c as a
# Windows x86_64 console program (lane C1-k5, docs/cp-k5.md).
#
# tools/cp/build-winprobe.sh's recipe unchanged (clang -target x86_64-windows,
# Wine 11.0's own headers and PE import libraries, winegcc as the link driver;
# no MinGW, no download).  Imports kernel32 + ntdll only.
#
# Env: WINE_BUILD (default ~/cp/wine-build: src/wine-11.0 + build-x86_64)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WB="${WINE_BUILD:-$HOME/cp/wine-build}"
SRC="$WB/src/wine-11.0"
OBJ="$WB/build-x86_64"
OUT="${1:-$ROOT/obj/cp-k5/thrstorm.exe}"
mkdir -p "$(dirname "$OUT")"
[ -d "$SRC/include" ] && [ -x "$OBJ/tools/winegcc/winegcc" ] ||
  { echo "build-thrstorm: no Wine build at $WB (tools/cp/build-wine.sh)" >&2; exit 2; }
O="${OUT%.exe}.o"
clang -c -o "$O" "$ROOT/src/user/cp/thrstorm.c" \
  -I"$OBJ/include" -I"$SRC/include" -I"$SRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD \
  -target x86_64-windows -fuse-ld=lld --no-default-config -fno-strict-aliasing \
  -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
(cd "$OBJ" && tools/winegcc/winegcc -o "$OUT" --wine-objdir . -b x86_64-windows -mconsole \
  "$O" dlls/winecrt0/x86_64-windows/libwinecrt0.a \
  libs/compiler-rt/x86_64-windows/libcompiler-rt.a dlls/ucrtbase/x86_64-windows/libucrtbase.a \
  dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a \
  --no-default-config -Wl,-debug:dwarf)
printf 'build-thrstorm: %s %d B sha256 %s\n' "$OUT" "$(stat -c %s "$OUT")" "$(sha256sum "$OUT" | cut -c1-16)"
