#!/usr/bin/env bash
# tools/cp/run-wine-smoke.sh - cp/sys Wine smoke inside HobbyOS.
#
# Stage the built Wine tree (tools/cp/build-wine.sh) and the glibc 2.31 runtime
# the way RimWorld stages a large userspace tree (NFS export + /LIB64 on the
# FAT disk), boot the intel guest MODE=test with src/user/wineprobe.c as the
# only wave member, and capture how far Wine gets.  See docs/cp-sys.md §8:
# the first run that needs low VA (ARCH_SET_GS / KUSER at 0x7ffe0000) is C1-mmu
# territory and is expected to fault; this records exactly where.
#
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE),
#      WINE_EXPORT (default /srv/nfs/wine-smoke; exported to 127.0.0.1 only --
#      QEMU user networking reaches the host from localhost; removed on exit),
#      TIMEOUT_S (default 240),
#      GLIBC231 (default /tmp/glibc231; run-rimworld.sh populates it).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
EXPORT="${WINE_EXPORT:-/srv/nfs/wine-smoke}"
G="${GLIBC231:-/tmp/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"
TIMEOUT_S="${TIMEOUT_S:-240}"
OUT="$ROOT/obj/wine-smoke"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
die() { echo "wine-smoke: $*" >&2; exit 2; }

for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -d "$STAGE/bin" ] || die "no staged Wine at $STAGE (run tools/cp/build-wine.sh all)"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR (run tools/rw/run-rimworld.sh once, or set GLIBC231)"

echo "== export: $EXPORT (CP/WINE copy)"
MADE_EXPORT_DIR=""
[ -e "$EXPORT" ] || MADE_EXPORT_DIR=1
sudo -n mkdir -p "$EXPORT/WINE"
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/"
sudo -n chmod -R a+rX "$EXPORT"
OPTS="ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534"
# /etc/exports is the host's own config: back it up, add one line, and put it
# back BYTE-FOR-BYTE on every exit path (trap), then re-export, so the host's
# export set is exactly what it was before this script ran.
EXP_BAK=""
restore_exports() {
  [ -n "$EXP_BAK" ] || return 0
  sudo -n cp -a "$EXP_BAK" /etc/exports && sudo -n exportfs -ra
  if sudo -n cmp -s "$EXP_BAK" /etc/exports; then
    echo "== /etc/exports restored (cmp identical to $EXP_BAK)"
  else
    echo "== WARNING: /etc/exports differs from $EXP_BAK"
  fi
}
# The export copy is removed again too, so nothing is left on the host.
cleanup() {
  restore_exports
  if [ -n "$MADE_EXPORT_DIR" ]; then
    sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  fi
}
trap cleanup EXIT
if ! grep -qsE "^[[:space:]]*${EXPORT//./\\.}[[:space:]]" /etc/exports; then
  EXP_BAK="/etc/exports.cp-sys-backup.$(date +%Y%m%d%H%M%S)"
  sudo -n cp -a /etc/exports "$EXP_BAK"
  printf '%s 127.0.0.1(%s)\n' "$EXPORT" "$OPTS" | sudo -n tee -a /etc/exports >/dev/null
fi
sudo -n exportfs -ra

echo "== build: kernel + disk (intel, test) + wineprobe.bin"
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=test hobbyos.elf disk.img \
  >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || exit 2
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
clang "${UCFLAGS[@]}" -DWINE_EXPORT="\"$EXPORT\"" -c src/user/wineprobe.c -o "$OUT/wineprobe.o" || exit 2
ld.lld -T src/user/linker.ld -e _start -o "$OUT/wineprobe.elf" "$OUT/wineprobe.o" obj/intel/libc.a || exit 2
llvm-objcopy -O binary "$OUT/wineprobe.elf" "$OUT/wineprobe.bin"

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld + wineprobe + WAVE.CFG"
mmd -i "$IMG" ::/LIB64 ::/lib64 ::/CP ::/tmp >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 libm-2.31.so:libm.so.6 \
         libpthread-2.31.so:libpthread.so.0 libdl-2.31.so:libdl.so.2 \
         librt-2.31.so:librt.so.1 libresolv-2.31.so:libresolv.so.2 libgcc_s.so.1:libgcc_s.so.1; do
  stage "$LDIR/${f%%:*}" "::/LIB64/${f##*:}"
done
stage "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/wineprobe.bin" ::/WPROBE.BIN
printf 'WPROBE.BIN\n' > "$OUT/wave.cfg"
stage "$OUT/wave.cfg" ::/WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
OVMF="$OUT/OVMF_CODE_4M.fd"; [ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "== boot (deadline ${TIMEOUT_S}s, under the shared QEMU lock)"
: >"$LOG"
flock /tmp/cp-qemu.lock bash -c '
  ( make -o disk.img ARCH=intel MODE=test EDK2_X86_64='"$OVMF"' run \
         QEMU_ARGS="-display none -enable-kvm" >>'"$LOG"' 2>&1 & )
  for _ in $(seq 1 '"$TIMEOUT_S"'); do
    grep -aq "System halt" '"$LOG"' && break
    grep -aq "\[WS\] done" '"$LOG"' && sleep 2 && break
    sleep 1
  done
  # Only the QEMU holding THIS tree'"'"'s disk.img (never a pattern kill:
  # other lanes'"'"' QEMUs share the box).
  for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done'
echo
echo "== wineprobe rows + the compat/loader ladder"
grep -anE '\[WS\]|\[LCOMPAT\]|error while loading|cannot open shared|failed to map|ARCH_SET_GS|KUSER|exit\(1\)|\[EXITPC\]|memory fault|PT_INTERP|wine:|wineserver:|System halt' "$LOG" || echo "(none)"
echo
echo "raw serial: $LOG"
