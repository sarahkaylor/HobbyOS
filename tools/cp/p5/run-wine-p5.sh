#!/usr/bin/env bash
# tools/cp/p5/run-wine-p5.sh - Gate P5, Wine rows (lane C5-vk2, docs/cp-vk.md §8):
# vkcube.exe (winevulkan), d3d11tri.exe (DXVK 3.1.1) and d3d12tri.exe
# (vkd3d-proton 3.1) on lavapipe, each in a window of the guest's X session
# for >= 60 s, two screendumps each, serial log.
#
# The Linux row (vkcubex) is tools/cp/p5/run_vkcubex.py (no NFS, no Wine).
#
# What it does (tools/cp/run-wine-p2.sh's pattern, moved to the X session):
#   1. builds the DEFAULT (desktop, DESKTOP=icewm) kernel + disk, the native
#      launcher tools/cp/p5/winep5.c -> WINEP5.BIN, and the PE probes
#      (tools/cp/p5/build-pe.sh);
#   2. exports ONE localhost-only read-only NFS dir with tools/cp/exports.sh
#      add/remove (docs/cyberpunk.md §7.4; `remove` runs in the EXIT trap, and
#      /tmp/cp-exports.lock is never held across the boot).  It holds WINE/
#      (CP_WINE_STAGE: a Wine built WITH X11 + Vulkan), X11/ (the client
#      libraries), VK/ (the x11 variant of the Vulkan tree, symlinks
#      dereferenced: NFS symlinks are unreliable in the guest) and P5/ (the
#      probes + ONLY the pinned x64 DLLs, docs/cp-pins.md).  The export dir is
#      removed on exit;
#   3. copies disk.img, stages glibc 2.31 (/LIB64 + /lib64/ld), /WINEP5.BIN and
#      /XSTART.TXT (XSESSION spawns the launcher once the session is up);
#   4. boot_wine_p5.py boots in one QEMU slot (tools/cp/qemu-slot.sh semantics,
#      the boot only), screendumps and grades.
#
# Env: CP_WINE_STAGE  (REQUIRED: <wine build>/stage/CP/WINE with winex11 + winevulkan)
#      WINE_BUILD     (Wine build tree for the PE headers/import libs; default ~/cp/wine-build)
#      CP_VK_WORK (default ~/cp/vk-build), CP_X11_WORK (default ~/cp/x11-build)
#      CP_P5_PE   (default ~/cp/vk2-pe: dxvk/*.dll, vkd3d/*.dll, the x64 DLLs only)
#      GLIBC231_LIB (default $CP_VK_WORK/sysroot-glibc231/lib/x86_64-linux-gnu)
#      P5_EXPORT (default /srv/nfs/cp-vk2-p5), P5_PROBES (default "vkcube d3d11 d3d12"),
#      P5_WINEDEBUG (default err+all,fixme-all), TIMEOUT_S (default 7200),
#      P5_CFLAGS (extra launcher -D flags, diagnostics only)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
WINE="${CP_WINE_STAGE:-}"
VKW="${CP_VK_WORK:-$HOME/cp/vk-build}"
X11W="${CP_X11_WORK:-$HOME/cp/x11-build}"
PE="${CP_P5_PE:-$HOME/cp/vk2-pe}"
LDIR="${GLIBC231_LIB:-$VKW/sysroot-glibc231/lib/x86_64-linux-gnu}"
EXPORT="${P5_EXPORT:-/srv/nfs/cp-vk2-p5}"
PROBES="${P5_PROBES:-vkcube d3d11 d3d12}"
WDBG="${P5_WINEDEBUG:-err+all,fixme-all}"
TIMEOUT_S="${TIMEOUT_S:-7200}"
OUT="$ROOT/obj/cp-p5/wine"; mkdir -p "$OUT"
IMG="$ROOT/disk.img"
die() { echo "wine-p5: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 rsync python3; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -n "$WINE" ] && [ -x "$WINE/bin/wine" ] || die "CP_WINE_STAGE must name a staged Wine with X11 + Vulkan (got '$WINE')"
[ -f "$WINE/lib/wine/x86_64-unix/winevulkan.so" ] || die "$WINE has no winevulkan.so (built --without-vulkan?)"
[ -f "$WINE/lib/wine/x86_64-unix/winex11.so" ] || die "$WINE has no winex11.so (built --without-x?)"
[ -f "$VKW/stage-x11/CP/VK/lib/libvulkan_lvp.so" ] || die "no x11 Vulkan tree (CP_VK_WSI=x11 tools/cp/vk/build.sh all)"
[ -d "$X11W/stage/CP/X11/lib" ] || die "no X11 stage at $X11W (tools/cp/x11/build.sh all)"
for f in dxvk/d3d11.dll dxvk/dxgi.dll dxvk/d3d10core.dll vkd3d/d3d12.dll vkd3d/d3d12core.dll; do
  [ -f "$PE/$f" ] || die "no $PE/$f (stage the pinned x64 DLLs, docs/cp-pins.md)"
done
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
case "$EXPORT" in /srv/nfs/?*) ;; *) die "P5_EXPORT must be under /srv/nfs/ ($EXPORT)";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete; remove it by hand"

echo "== build: kernel + disk (intel, default desktop/icewm), WINEP5.BIN, PE probes"
make ARCH=intel hobbyos.elf disk.img >"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; die "kernel/disk build"; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
# P5_CFLAGS: extra -D flags for the launcher (diagnostics, e.g. -DP5_WINEBOOT_NODISPLAY)
# shellcheck disable=SC2086
clang "${UCFLAGS[@]}" -DP5_EXPORT="\"$EXPORT\"" -DP5_WINEDEBUG="\"$WDBG\"" ${P5_CFLAGS:-} \
  -c tools/cp/p5/winep5.c -o "$OUT/winep5.o" || die "winep5.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/winep5.elf" "$OUT/winep5.o" obj/intel/libc.a || die "ld winep5"
llvm-objcopy -O binary "$OUT/winep5.elf" "$OUT/winep5.bin"
CP_VK_WORK="$VKW" bash tools/cp/p5/build-pe.sh "$OUT/pe" >"$OUT/pe-build.log" 2>&1 ||
  { cat "$OUT/pe-build.log"; die "PE probes"; }
cat "$OUT/pe-build.log"

# ---- the export: tools/cp/exports.sh add/remove only (docs/cyberpunk.md §7.4);
# /tmp/cp-exports.lock is held by exports.sh for each edit, never across the boot.
MADE_DIR=""
ADDED=""
CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  if [ -n "$ADDED" ]; then
    bash "$ROOT/tools/cp/exports.sh" remove "$EXPORT" | sed 's/^/== /'
  fi
  if [ -n "$MADE_DIR" ]; then sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"; fi
  sudo -n exportfs -v 2>/dev/null | grep -F "$EXPORT" && echo "== WARNING: $EXPORT still exported"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

echo "== export dir: $EXPORT (WINE, X11, VK, P5)"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/X11" "$EXPORT/VK" "$EXPORT/P5/VK" "$EXPORT/P5/D3D11" "$EXPORT/P5/D3D12" && MADE_DIR=1
sudo -n rsync -a --delete "$WINE/" "$EXPORT/WINE/" || die "rsync wine"
sudo -n rsync -aL --delete "$X11W/stage/CP/X11/" "$EXPORT/X11/" || die "rsync x11"
sudo -n rsync -aL --delete --exclude include --exclude cmake --exclude pkgconfig \
  "$VKW/stage-x11/CP/VK/" "$EXPORT/VK/" || die "rsync vk"
sudo -n cp "$OUT/pe/vkcube.exe" "$EXPORT/P5/VK/"
sudo -n cp "$OUT/pe/d3d11tri.exe" "$PE/dxvk/d3d11.dll" "$PE/dxvk/dxgi.dll" "$PE/dxvk/d3d10core.dll" "$EXPORT/P5/D3D11/"
sudo -n cp "$OUT/pe/d3d12tri.exe" "$PE/vkd3d/d3d12.dll" "$PE/vkd3d/d3d12core.dll" "$PE/dxvk/dxgi.dll" "$EXPORT/P5/D3D12/"
sudo -n chmod -R a+rX "$EXPORT"
( cd "$EXPORT" && find P5 VK/lib VK/share -type f | sort | while read -r f; do
    printf '%s %10d %s\n' "$(sha256sum "$f" | cut -c1-16)" "$(stat -c %s "$f")" "$f"; done ) | tee "$OUT/export-manifest.txt"
# ADDED before the add: a half-finished add is still undone by the trap
# (exports.sh remove is a no-op when the line is absent).
ADDED=1
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /'
[ "${PIPESTATUS[0]}" = 0 ] || die "exports.sh add $EXPORT failed"
sudo -n exportfs -v | grep -F "$EXPORT" | sed 's/^/== exported: /'

echo "== stage (a copy of disk.img): glibc 2.31 /LIB64 + /lib64/ld, /WINEP5.BIN, /XSTART.TXT"
D="$OUT/disk.img"; cp "$IMG" "$D"
mmd -i "$D" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$D" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/winep5.bin" ::/WINEP5.BIN
printf '# autostart (staged by tools/cp/p5/run-wine-p5.sh)\n/WINEP5.BIN %s\n' "$PROBES" > "$OUT/XSTART.TXT"
stage "$OUT/XSTART.TXT" ::/XSTART.TXT

echo "== boot + grade (deadline ${TIMEOUT_S}s; one QEMU slot for the boot only)"
python3 tools/cp/p5/boot_wine_p5.py --out "$OUT" --disk "$D" --probes "${PROBES// /,}" --timeout "$TIMEOUT_S"
rc=$?
echo "raw serial: $OUT/serial.log"
exit $rc
