#!/usr/bin/env bash
# tools/cp/p8/run-p8.sh - the P8 launch recipe (lane C8-game, docs/cp-p8.md):
# Cyberpunk 2077 in a HobbyOS X session on lavapipe, Wine 11 + vkd3d-proton 3.1
# (d3d12, d3d12core) + DXVK 3.1.1 (dxgi, d3d11) as native overrides.
#
#   P8_PHASE=pfx   build the prefix image once: a fresh tools/cp/mkcpdata.sh
#                  ext2 disk, one boot running `wineboot -i` with
#                  WINEPREFIX=/EXT/pfx (+ WineDbg ShowCrashDialog=0, sync),
#                  e2fsck -fn, then tools/cp/p8/seed-prefix.sh (the DLLs into
#                  system32, the user's first-run profile from
#                  ~/cp/game-profile, the cache dirs), e2fsck -fn again.
#                  Result: obj/cp-p8/pfx-seeded.img (kept; every game boot
#                  runs on a COPY of it).
#   P8_PHASE=probe the kernel probes only (src/user/cp/p8*probe.c,
#                  p8fdcount.c; docs/cp-p8.md §4), no Wine, no game export.
#   P8_PHASE=game  (default) one game boot on a copy of the seeded image:
#                  keepalive/warm-up (winep5's), then from the read-only NFS
#                  CWD /GAME/bin/x64:
#                    wine Z:\GAME\bin\x64\Cyberpunk2077.exe --launcher-skip
#                  boot_p8.py screendumps, OCRs, stops at the main menu, at a
#                  first-run screen (never answered), at a stall or timeout.
#
# Exports (docs/cyberpunk.md §7.4): ONLY through tools/cp/exports.sh, both
# removed in the EXIT trap.  P8_EXPORT (default /srv/nfs/cp-game-p8) is this
# run's own staging dir (WINE/, X11/, VK/, P5/VK/vkcube.exe), created and
# deleted here.  /srv/nfs/cp-game is the user's read-only game copy: it is
# exported (game phase only) and un-exported, NEVER written, chmod'ed, copied
# or deleted; its file count / bytes / newest mtime are recorded before and
# after the boot to show it did not change.
#
# Env: CP_WINE_STAGE (default ~/cp/wine-build-x/stage/CP/WINE), WINE_BUILD
#      (default ~/cp/wine-build-x), CP_VK_WORK (~/cp/vk2-build), CP_X11_WORK
#      (~/cp/x11-build-x2), CP_P5_PE (~/cp/vk2-pe), P8_PROFILE (~/cp/game-profile),
#      P8_XHOBBY + P8_XHOBBY_SHA256 (a scratch X server over the tree's own
#      XHOBBY.BIN; default none since integ5/C4-x3), P8_RUN (run name,
#      default run<N>), P8_WINEDEBUG, P8_GAME_S (launcher's game deadline,
#      default 3600), TIMEOUT_S (boot deadline, default 5400), P8_STALL_S
#      (default 1200), P8_PRESS_CONTINUE (default 1), P8_CFLAGS (extra -D for
#      the launcher, e.g. -DP8_EXTRA_ENV='"VKD3D_FEATURE_LEVEL=12_0"'),
#      P8_SMP (default 8), P8_MEM (MiB, default 12288, never more),
#      P8_VKD3D_FL (VKD3D_FEATURE_LEVEL; default 12_0 on lavapipe, unset on Venus),
#      P8_GPU (lavapipe | venus), VENUS_BUILD (~/cp/venus-build), HOSTMEM (Venus
#      BAR window, host-venus.sh default 8G: a host mapping, not guest RAM).
# P9 hooks (lane C9-play, docs/cp-p9.md; tools/cp/p9/run-p9.sh sets them):
#      P8_BASE (run dir parent, default obj/cp-p8), P8_SEEDED (the seeded image a
#      game boot copies, default $P8_BASE/pfx-seeded.img), P8_CPDATA_FROM (copy
#      THIS data image instead, e.g. the previous P9 boot's cpdata.img with the
#      game's own save on it), P8_BOOT (the boot/watch script, default
#      tools/cp/p8/boot_p8.py), P8_BOOT_ARGS (extra args for it), P8_MEM above
#      12288 (P9 game boots only, at most 32768: the boot script takes
#      /tmp/cp-bigguest.lock inside the slot, docs/cyberpunk.md §7.2f).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
PHASE="${P8_PHASE:-game}"
WINE="${CP_WINE_STAGE:-$HOME/cp/wine-build-x/stage/CP/WINE}"
export WINE_BUILD="${WINE_BUILD:-$HOME/cp/wine-build-x}"
VKW="${CP_VK_WORK:-$HOME/cp/vk2-build}"
X11W="${CP_X11_WORK:-$HOME/cp/x11-build-x2}"
PE="${CP_P5_PE:-$HOME/cp/vk2-pe}"
PROF="${P8_PROFILE:-$HOME/cp/game-profile}"
LDIR="${GLIBC231_LIB:-$VKW/sysroot-glibc231/lib/x86_64-linux-gnu}"
EXPORT="${P8_EXPORT:-/srv/nfs/cp-game-p8}"
GAME=/srv/nfs/cp-game
# integ5 (C4-x3): the tree builds its own one-screen XHOBBY.BIN with MIT-SHM;
# P8_XHOBBY (+ P8_XHOBBY_SHA256) stages a scratch server over it, diagnostics only.
XHOBBY="${P8_XHOBBY:-}"
XHOBBY_SHA="${P8_XHOBBY_SHA256:-}"
WDBG="${P8_WINEDEBUG:-+loaddll,fixme-all,+fps}"   # +fps: winevulkan's "N fps" row every few seconds
# P8_GPU=lavapipe (default) | venus (docs/cp-p8.md §9): Venus reuses C6-venus's
# pieces unchanged -- tools/cp/host-venus.sh's QEMU line and environment
# (virtio-gpu-gl venus/blob/hostmem, memfd guest RAM, egl-headless on
# renderD129, the dma-buf/deny shim, MESA_VK_DEVICE_SELECT) and the guest ICD
# from ~/cp/venus-build (tools/mesa/venus/build.sh: present_wait hidden,
# synchronous present).  The guest Vulkan tree is C5's loader with lavapipe's
# ICD JSON removed and the Venus ICD added (tools/cp/venus/run-wine-venus.sh's
# merge), so Venus is the only GPU.
GPU="${P8_GPU:-lavapipe}"
VENUS_BUILD="${VENUS_BUILD:-$HOME/cp/venus-build}"
TIMEOUT_S="${TIMEOUT_S:-5400}"
BASE="${P8_BASE:-$ROOT/obj/cp-p8}"
SEEDED="${P8_SEEDED:-$BASE/pfx-seeded.img}"
BOOT="${P8_BOOT:-tools/cp/p8/boot_p8.py}"
MEM_MIB="${P8_MEM:-12288}"
case "$MEM_MIB" in *[!0-9]*|'') echo "run-p8: P8_MEM must be MiB" >&2; exit 2;; esac
[ "$MEM_MIB" -le 32768 ] || { echo "run-p8: P8_MEM above 32768 MiB is not approved" >&2; exit 2; }
IMG="$ROOT/disk.img"
die() { echo "run-p8: $*" >&2; exit 2; }
case "$GPU" in lavapipe|venus) ;; *) die "P8_GPU must be lavapipe or venus";; esac
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"
export PATH="/usr/sbin:/sbin:$PATH"
mkdir -p "$BASE"
case "$PHASE" in pfx|game|probe) ;; *) die "P8_PHASE must be pfx, game or probe";; esac
if [ "$PHASE" = pfx ]; then RUN="${P8_RUN:-pfx}"; elif [ "$PHASE" = probe ]; then
  n=1; while [ -e "$BASE/probe$n" ]; do n=$((n+1)); done; RUN="${P8_RUN:-probe$n}"; else
  n=1; while [ -e "$BASE/run$n" ]; do n=$((n+1)); done; RUN="${P8_RUN:-run$n}"; fi
OUT="$BASE/$RUN"; mkdir -p "$OUT"
exec > >(tee -a "$OUT/run-p8.log") 2>&1
echo "== run-p8 gpu=$GPU phase=$PHASE run=$RUN $(date -Is) host=$(hostname) tree=$ROOT"

for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 rsync python3 musl-gcc debugfs e2fsck mke2fs; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$WINE/bin/wine" ] || die "no staged Wine at $WINE"
[ -f "$WINE/lib/wine/x86_64-unix/winevulkan.so" ] && [ -f "$WINE/lib/wine/x86_64-unix/winex11.so" ] ||
  die "$WINE lacks winevulkan.so / winex11.so"
[ -f "$VKW/stage-x11/CP/VK/lib/libvulkan_lvp.so" ] || die "no x11 Vulkan tree in $VKW"
[ -d "$X11W/stage/CP/X11/lib" ] || die "no X11 stage at $X11W"
for f in dxvk/d3d11.dll dxvk/dxgi.dll vkd3d/d3d12.dll vkd3d/d3d12core.dll; do
  [ -f "$PE/$f" ] || die "no $PE/$f"
done
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
case "$EXPORT" in /srv/nfs/cp-?*) ;; *) die "P8_EXPORT must be /srv/nfs/cp-<name> ($EXPORT)";; esac
[ "$EXPORT" != "$GAME" ] || die "P8_EXPORT must not be the game copy"
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete; remove it by hand"
if [ "$MEM_MIB" -gt 12288 ] && { [ "$PHASE" != game ] || [ "$BOOT" = tools/cp/p8/boot_p8.py ]; }; then
  die "P8_MEM=$MEM_MIB: guests above 12 GiB are for P9 game boots only (P8_BOOT=tools/cp/p9/boot_p9.py)"
fi
if [ "$PHASE" = game ]; then
  if [ -n "${P8_CPDATA_FROM:-}" ]; then [ -f "$P8_CPDATA_FROM" ] || die "no P8_CPDATA_FROM=$P8_CPDATA_FROM"
  else [ -f "$SEEDED" ] || die "no $SEEDED: run P8_PHASE=pfx first"; fi
  [ -f "$GAME/bin/x64/Cyberpunk2077.exe" ] || die "no game copy at $GAME"
fi

echo "== build: kernel + disk (intel, desktop/icewm), P8.BIN, PFXSYNC.ELF, PE warm-up client"
make ARCH=intel hobbyos.elf disk.img >"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; die "kernel/disk build"; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
PH=2; [ "$PHASE" = pfx ] && PH=1; [ "$PHASE" = probe ] && PH=3
# lavapipe needs VKD3D_FEATURE_LEVEL=12_0 (docs/cp-p8.md stop 4); the RX 9070 XT
# reports 12_x natively, so Venus runs without it.  P8_VKD3D_FL= unsets.
if [ "$GPU" = venus ]; then VKFL="${P8_VKD3D_FL-}"; else VKFL="${P8_VKD3D_FL-12_0}"; fi
# shellcheck disable=SC2086
clang "${UCFLAGS[@]}" -DP8_PHASE=$PH -DP8_EXPORT="\"$EXPORT\"" -DP8_WINEDEBUG="\"$WDBG\"" \
  ${P8_GAME_S:+-DP8_GAME_S=$P8_GAME_S} ${VKFL:+-DP8_VKD3D_FL="\"$VKFL\""} ${P8_CFLAGS:-} \
  -c tools/cp/p8/p8launch.c -o "$OUT/p8launch.o" || die "p8launch.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/p8launch.elf" "$OUT/p8launch.o" obj/intel/libc.a || die "ld p8launch"
llvm-objcopy -O binary "$OUT/p8launch.elf" "$OUT/p8.bin"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/pfxsync.elf" tools/cp/pfx/pfxsync.c || die "pfxsync"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8env.elf" src/user/cp/p8envprobe.c || die "p8envprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8fd.elf" src/user/cp/p8fdcount.c || die "p8fdcount"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8scm.elf" src/user/cp/p8scmprobe.c || die "p8scmprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8kill.elf" src/user/cp/p8killprobe.c -lpthread || die "p8killprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p9fork.elf" src/user/cp/p9forkprobe.c -lpthread || die "p9forkprobe"
if [ "$PHASE" = game ]; then
  CP_VK_WORK="$VKW" bash tools/cp/p5/build-pe.sh "$OUT/pe" >"$OUT/pe-build.log" 2>&1 ||
    { cat "$OUT/pe-build.log"; die "PE warm-up client"; }
  grep 'vkcube.exe ' "$OUT/pe-build.log"
  # src/user/cp/p8d3d12caps.c with tools/cp/p5/build-pe.sh's recipe (clang PE
  # + winegcc against the Wine build's import libs; no MinGW, no download)
  WSRC="$WINE_BUILD/src/wine-11.0"; WOBJ="$WINE_BUILD/build-x86_64"
  clang -c -o "$OUT/pe/p8caps.o" src/user/cp/p8d3d12caps.c -I"$WOBJ/include" -I"$WSRC/include" \
    -I"$WSRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD -target x86_64-windows -fuse-ld=lld \
    --no-default-config -fno-strict-aliasing -O1 -g -Wall -Wno-unused-parameter || die "p8d3d12caps.c"
  (cd "$WOBJ" && tools/winegcc/winegcc -o "$OUT/pe/p8caps.exe" --wine-objdir . -b x86_64-windows -mconsole \
    "$OUT/pe/p8caps.o" dlls/winecrt0/x86_64-windows/libwinecrt0.a libs/compiler-rt/x86_64-windows/libcompiler-rt.a \
    dlls/ucrtbase/x86_64-windows/libucrtbase.a dlls/user32/x86_64-windows/libuser32.a \
    dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a \
    --no-default-config -Wl,-debug:dwarf) >"$OUT/pe/p8caps.log" 2>&1 ||
    { cat "$OUT/pe/p8caps.log"; die "p8caps.exe link"; }
  echo "== p8caps.exe $(stat -c %s "$OUT/pe/p8caps.exe") B sha256 $(sha256sum "$OUT/pe/p8caps.exe" | cut -c1-16)"
fi

# game copy census (never writes: find/stat only)
game_census() {
  find "$GAME" -type f -printf '%s %T@\n' | awk -v tag="$1" '{n++; b+=$1; if ($2>m) m=$2} END {printf "== game copy %s: %d files, %d bytes, newest mtime %.0f\n", tag, n, b, m}'
  printf '== game copy %s: modes of the top dir and bin/x64: %s %s\n' "$1" "$(stat -c %A "$GAME")" "$(stat -c %A "$GAME/bin/x64")"
}

# The game copy's export is SHARED by every concurrent P8 run (C3-ext2w runs
# this harness too).  Run 14 lost it mid-boot: another run's `exports.sh add`
# failed ("already exported"), its EXIT trap removed the line, and this run's
# guest got ENOENT for Z:\GAME (docs/cp-p8.md §9, stop 10).  So the export is
# reference-counted: under /tmp/cp-game-export.lock each run registers its pid
# in /tmp/cp-game-export.users/, the first one adds the export and the last
# one (dead pids pruned) removes it.  The lock is held only around the edit.
GLOCK=/tmp/cp-game-export.lock
GUSERS=/tmp/cp-game-export.users
game_export_get() {
  (
    flock 197
    mkdir -p "$GUSERS"
    : > "$GUSERS/$$"
    if grep -qxF "$GAME 127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)" /etc/exports; then
      echo "== $GAME already exported (shared with: $(ls "$GUSERS" | grep -vx "$$" | tr '\n' ' '))"
    else
      bash "$ROOT/tools/cp/exports.sh" add "$GAME" | sed 's/^/== /'
      [ "${PIPESTATUS[0]}" = 0 ] || { rm -f "$GUSERS/$$"; exit 1; }
    fi
  ) 197>>"$GLOCK"
}
game_export_put() {
  (
    flock 197
    rm -f "$GUSERS/$$"
    for f in "$GUSERS"/*; do [ -e "$f" ] || continue; kill -0 "${f##*/}" 2>/dev/null || rm -f "$f"; done
    if [ -z "$(ls -A "$GUSERS" 2>/dev/null)" ]; then
      bash "$ROOT/tools/cp/exports.sh" remove "$GAME" | sed 's/^/== /'
    else
      echo "== $GAME left exported for: $(ls "$GUSERS" | tr '\n' ' ')"
    fi
  ) 197>>"$GLOCK"
}

MADE_DIR=""; ADDED=""; ADDED_GAME=""; CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  [ -n "$ADDED_GAME" ] && game_export_put
  [ -n "$ADDED" ] && bash "$ROOT/tools/cp/exports.sh" remove "$EXPORT" | sed 's/^/== /'
  if [ -n "$MADE_DIR" ]; then sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"; fi
  sudo -n exportfs -v 2>/dev/null | grep -E "^$EXPORT[[:space:]]" && echo "== WARNING: $EXPORT still exported"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

echo "== staging export: $EXPORT (WINE, X11, VK, P5/VK)"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/X11" "$EXPORT/VK" "$EXPORT/P5/VK" && MADE_DIR=1
sudo -n rsync -a --delete "$WINE/" "$EXPORT/WINE/" || die "rsync wine"
sudo -n rsync -aL --delete "$X11W/stage/CP/X11/" "$EXPORT/X11/" || die "rsync x11"
sudo -n rsync -aL --delete --exclude include --exclude cmake --exclude pkgconfig \
  "$VKW/stage-x11/CP/VK/" "$EXPORT/VK/" || die "rsync vk"
if [ "$GPU" = venus ]; then
  [ -f "$VENUS_BUILD/stage/CP/VK/lib/libvulkan_virtio.so" ] || die "no Venus ICD in $VENUS_BUILD (tools/mesa/venus/build.sh)"
  sudo -n rm -f "$EXPORT/VK/share/vulkan/icd.d/lvp_icd.x86_64.json" "$EXPORT/VK/lib/libvulkan_lvp.so"
  sudo -n cp "$VENUS_BUILD/stage/CP/VK/lib/libvulkan_virtio.so" "$EXPORT/VK/lib/" &&
    sudo -n cp "$VENUS_BUILD/stage/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json" "$EXPORT/VK/share/vulkan/icd.d/" ||
    die "stage the Venus ICD"
  echo "== Vulkan ICDs in the export: $(ls "$EXPORT/VK/share/vulkan/icd.d" | tr '\n' ' ')(Venus only)"
fi
if [ "$PHASE" = game ]; then
  sudo -n mkdir -p "$EXPORT/P8" && sudo -n cp "$OUT/pe/vkcube.exe" "$EXPORT/P5/VK/" &&
    sudo -n cp "$OUT/pe/p8caps.exe" "$EXPORT/P8/" || die "stage the PE probes"
fi
sudo -n chmod -R a+rX "$EXPORT"
( cd "$EXPORT" && find P5 P8 VK/lib VK/share -type f | sort | while read -r f; do
    printf '%s %10d %s\n' "$(sha256sum "$f" | cut -c1-16)" "$(stat -c %s "$f")" "$f"; done ) > "$OUT/export-manifest.txt"
ADDED=1
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /'
[ "${PIPESTATUS[0]}" = 0 ] || die "exports.sh add $EXPORT failed"
if [ "$PHASE" = game ]; then
  game_census before | tee "$OUT/game-census-before.txt"
  game_export_get || die "exporting $GAME failed"
  ADDED_GAME=1
fi
sudo -n exportfs -v | grep -E "^($EXPORT|$GAME)[[:space:]]" | sed 's/^/== exported: /'

echo "== stage (a copy of disk.img): glibc 2.31, /P8.BIN, /PFXSYNC.ELF, XHOBBY.BIN, XSESSION.ENV, /XSTART.TXT"
D="$OUT/disk.img"; cp "$IMG" "$D"
mmd -i "$D" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$D" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/p8.bin" ::/P8.BIN
stage "$OUT/pfxsync.elf" ::/PFXSYNC.ELF
stage "$OUT/p8env.elf" ::/P8ENV.ELF
stage "$OUT/p8fd.elf" ::/P8FD.ELF
stage "$OUT/p8scm.elf" ::/P8SCM.ELF
stage "$OUT/p8kill.elf" ::/P8KILL.ELF
stage "$OUT/p9fork.elf" ::/P9FORK.ELF
if [ -n "$XHOBBY" ]; then
  [ -n "$XHOBBY_SHA" ] || die "P8_XHOBBY needs P8_XHOBBY_SHA256"
  cp "$XHOBBY" "$OUT/XHOBBY.BIN"
  echo "$XHOBBY_SHA  $OUT/XHOBBY.BIN" | sha256sum -c --quiet - || die "XHOBBY.BIN sha256 mismatch"
  stage "$OUT/XHOBBY.BIN" ::/XHOBBY.BIN
  echo "== X server: $XHOBBY (sha256 $XHOBBY_SHA) staged as ::/XHOBBY.BIN (scratch override, not committed)"
else
  echo "== X server: the tree's own XHOBBY.BIN, sha256 $(mcopy -n -i "$D" ::/XHOBBY.BIN - 2>/dev/null | sha256sum | cut -c1-16)"
fi
rm -f "$OUT/XSESSION.ENV"
mcopy -o -i "$D" ::/X11/XSESSION.ENV "$OUT/XSESSION.ENV" 2>/dev/null || : > "$OUT/XSESSION.ENV"
printf 'HB_SECOND_SCREEN=0\n' >> "$OUT/XSESSION.ENV"
stage "$OUT/XSESSION.ENV" ::/X11/XSESSION.ENV
printf '# autostart (staged by tools/cp/p8/run-p8.sh)\n/P8.BIN\n' > "$OUT/XSTART.TXT"
stage "$OUT/XSTART.TXT" ::/XSTART.TXT

if [ "$PHASE" = pfx ] || [ "$PHASE" = probe ]; then
  CPD="$OUT/cpdata.img"
  CPDATA_FORCE=1 bash tools/cp/mkcpdata.sh "$CPD" 16G || die "mkcpdata"
else
  CPD="$OUT/cpdata.img"
  SRC_IMG="${P8_CPDATA_FROM:-$SEEDED}"
  cp --sparse=always "$SRC_IMG" "$CPD"
  echo "== data disk: a copy of $SRC_IMG"
fi

echo "== boot (deadline ${TIMEOUT_S}s; one QEMU slot for the boot only)"
BPH="$PHASE"; [ "$PHASE" = probe ] && BPH=pfx   # the same "wait for [P8] done" boot
BA=(--out "$OUT" --disk "$D" --cpdata "$CPD" --phase "$BPH" --timeout "$TIMEOUT_S" --gpu "$GPU"
    --smp "${P8_SMP:-8}" --mem "$MEM_MIB" --stall-s "${P8_STALL_S:-1200}")
[ "${P8_PRESS_CONTINUE:-1}" = 1 ] && BA+=(--press-continue)
# shellcheck disable=SC2086
python3 "$BOOT" "${BA[@]}" ${P8_BOOT_ARGS:-}
rc=$?
tr -d '\r' < "$OUT/serial.log" > "$OUT/serial.txt" 2>/dev/null

e2fsck -fn "$CPD" > "$OUT/fsck.log" 2>&1; frc=$?
echo "== e2fsck -fn after the boot: rc=$frc ($(tail -1 "$OUT/fsck.log"))"
if [ "$PHASE" = pfx ]; then
  grep -q '\[P8\] sync done' "$OUT/serial.txt" || die "the prefix boot did not reach sync (see $OUT/serial.txt)"
  [ "$frc" = 0 ] || die "e2fsck after the prefix boot rc=$frc"
  bash tools/cp/p8/seed-prefix.sh "$CPD" "$PE" "$PROF" tools/cp/p0host/settings-low-native.keys "$OUT/seed.log" ||
    die "seed-prefix"
  e2fsck -fn "$CPD" > "$OUT/fsck-seeded.log" 2>&1 || die "e2fsck after seeding: $(tail -3 "$OUT/fsck-seeded.log")"
  echo "== e2fsck -fn after seeding: rc=0 ($(tail -1 "$OUT/fsck-seeded.log"))"
  cp --sparse=always "$CPD" "$SEEDED"
  echo "== seeded prefix image: $SEEDED"
elif [ "$PHASE" = game ]; then
  game_census after | tee "$OUT/game-census-after.txt"
  if [ "$(head -1 "$OUT/game-census-before.txt" | cut -d: -f2-)" = "$(head -1 "$OUT/game-census-after.txt" | cut -d: -f2-)" ]; then
    echo "== game copy unchanged (count, bytes, newest mtime)"
  else
    echo "== WARNING: game copy census changed"
  fi
  echo "== caches on ext2 after the boot (debugfs, sizes only)"
  for d in /pfx/drive_c/vc /mc; do
    debugfs -R "ls -l $d" "$CPD" 2>/dev/null | awk -v d="$d" 'NF>=8 && $NF!="." && $NF!=".." {n++; b+=$6} END {printf "   %-28s %d entries, %d bytes\n", d, n, b}'
  done
  echo "== verdict: $(cat "$OUT/verdict.txt" 2>/dev/null)"
  echo "== timeline:"; sed 's/^/   /' "$OUT/timeline.txt"
fi
echo "raw serial: $OUT/serial.txt"
exit $rc
