#!/usr/bin/env bash
# tools/cp/p5/build-pe.sh [OUTDIR] -- build the Gate P5 Windows probes (lane
# C5-vk2, docs/cp-vk.md §8) as x86_64 PE console programs:
#
#   vkcube.exe     src/user/cp/vkcubex.c   (VK_KHR_win32_surface; vulkan-1.dll)
#   d3d11tri.exe   src/user/cp/d3d11tri.c  (DXVK d3d11 + dxgi)
#   d3d12tri.exe   src/user/cp/d3d12tri.c  (vkd3d-proton d3d12 + DXVK dxgi)
#
# Exactly tools/cp/build-winprobe.sh's recipe (no MinGW, no download): clang 21
# -target x86_64-windows with Wine's own Windows headers, linked by winegcc
# against the PE import libraries the Wine 11.0 build produced.  The Vulkan
# headers are the pinned Vulkan-Headers (vulkan-sdk-1.4.363.0) that
# tools/cp/vk/build.sh installs.  Only kernel32/user32/gdi32/ucrtbase are
# imported: every D3D/Vulkan entry point is resolved with LoadLibrary +
# GetProcAddress at run time (the probes log which module answered).
#
# Env: WINE_BUILD  (default ~/cp/wine-build: src/wine-11.0 + build-x86_64)
#      CP_VK_WORK  (default ~/cp/vk-build: vulkan-headers/include)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WB="${WINE_BUILD:-$HOME/cp/wine-build}"
SRC="$WB/src/wine-11.0"
OBJ="$WB/build-x86_64"
VKH="${CP_VK_WORK:-$HOME/cp/vk-build}/vulkan-headers/include"
OUT="${1:-$ROOT/obj/cp-p5/pe}"
mkdir -p "$OUT"
[ -d "$SRC/include" ] && [ -x "$OBJ/tools/winegcc/winegcc" ] ||
  { echo "build-pe: no Wine build at $WB (tools/cp/build-wine.sh)" >&2; exit 2; }
[ -f "$VKH/vulkan/vulkan.h" ] || { echo "build-pe: no Vulkan-Headers at $VKH (tools/cp/vk/build.sh loader)" >&2; exit 2; }

CF=(-I"$OBJ/include" -I"$SRC/include" -I"$SRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD
    -target x86_64-windows -fuse-ld=lld --no-default-config -fno-strict-aliasing
    -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
LIBS=(dlls/winecrt0/x86_64-windows/libwinecrt0.a libs/compiler-rt/x86_64-windows/libcompiler-rt.a
      dlls/ucrtbase/x86_64-windows/libucrtbase.a dlls/user32/x86_64-windows/libuser32.a
      dlls/gdi32/x86_64-windows/libgdi32.a dlls/kernel32/x86_64-windows/libkernel32.a
      dlls/ntdll/x86_64-windows/libntdll.a)

build() { # $1 source  $2 exe  [extra cflags...]
  local src="$1" exe="$2"; shift 2
  local o="$OUT/${exe%.exe}.o"
  clang -c -o "$o" "$ROOT/src/user/cp/$src" "${CF[@]}" "$@"
  (cd "$OBJ" && tools/winegcc/winegcc -o "$OUT/$exe" --wine-objdir . -b x86_64-windows -mconsole \
    "$o" "${LIBS[@]}" --no-default-config -Wl,-debug:dwarf)
  printf 'build-pe: %-13s %8d B  sha256 %s\n' "$exe" "$(stat -c %s "$OUT/$exe")" \
    "$(sha256sum "$OUT/$exe" | cut -c1-16)"
}

build vkcubex.c  vkcube.exe   -I"$VKH" -I"$ROOT/src/user/cp"
build d3d11tri.c d3d11tri.exe
build d3d12tri.c d3d12tri.exe
for e in vkcube d3d11tri d3d12tri; do
  echo "build-pe: $e.exe imports: $(llvm-objdump -p "$OUT/$e.exe" 2>/dev/null | sed -n 's/^ *DLL Name: //p' | tr '\n' ' ')"
done
