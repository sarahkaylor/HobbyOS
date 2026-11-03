#!/usr/bin/env bash
# tools/rw/run-rwnfs-probe.sh — build and run src/user/rwnfs_probe.c in the
# intel guest, with the RimWorld NFS export live.
#
# The Makefile is L0-owned, so this probe is built here with the same flags
# the Makefile uses for user programs (USER_CFLAGS: Makefile:279/288) and
# linked against the tree's own obj/intel/libc.a.  If those flags change and
# this drifts, the compile line below is the one to fix; `make obj/intel/libc.a`
# keeps the libc side honest.
#
# Usage: tools/rw/run-rwnfs-probe.sh [--timeout SECS]
# Env:   RW_NFS_SERVER   host address the guest sees (default 10.0.2.2)
#        RW_NFS_ALT      fallback host LAN address (default 192.168.0.123)
#        RW_SKIP_EXPORT=1  do not run tools/rw/nfs-export.sh sync first
#
# Exit: 0 the probe reached "done failures=0" with the M5 markers present,
#       1 it did not (the console log says which line), 2 environment.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
TIMEOUT_S=300
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S="$2"; shift 2 ;;
    *) echo "usage: $0 [--timeout SECS]" >&2; exit 2 ;;
  esac
done

SERVER="${RW_NFS_SERVER:-10.0.2.2}"
ALT="${RW_NFS_ALT:-192.168.0.123}"
OUT="$ROOT/obj/rwnfs_probe"
LOG="$OUT/serial.log"
mkdir -p "$OUT"

[ -f src/user/rwnfs_probe.c ] || { echo "missing src/user/rwnfs_probe.c"; exit 2; }

# The Makefile resolves OVMF as $(HOME)/.local/share/OVMF/OVMF_CODE_4M.fd.  A
# session whose HOME is a scratch dir would otherwise die in QEMU with
# "Could not open .../OVMF_CODE_4M.fd" *after* a full build (measured).  Fall
# back to the real account home when the current one has no OVMF.
if [ ! -f "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" ] && \
   [ -f "/home/$(id -un)/.local/share/OVMF/OVMF_CODE_4M.fd" ]; then
  HOME="/home/$(id -un)"; export HOME
  echo "note: HOME was a scratch dir; using $HOME for the Makefile's OVMF path"
fi

PATH="/usr/sbin:/sbin:$PATH"; export PATH
for t in clang ld.lld llvm-objcopy mcopy mkfs.fat qemu-system-x86_64; do
  command -v "$t" >/dev/null 2>&1 || { echo "ENVIRONMENT: missing $t"; exit 2; }
done

echo "== build: libc.a (make, the tree's own rule)"
make -s ARCH=intel obj/intel/libc.a >"$OUT/build_libc.log" 2>&1 || {
  echo "BUILD FAILED (libc.a)"; tail -30 "$OUT/build_libc.log"; exit 1; }

RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large
         -DRW_NFS_SERVER="\"$SERVER\"" -DRW_NFS_ALT="\"$ALT\"")

build_one() { # src basename [linker-script]
  local src="$1" base="$2" ldscript="${3:-src/user/linker.ld}"
  echo "== build: $src ($ldscript)"
  clang "${UCFLAGS[@]}" -c "$src" -o "$OUT/$base.o" || return 1
  ld.lld -T "$ldscript" -e _start -o "$OUT/$base.elf" "$OUT/$base.o" obj/intel/libc.a || return 1
}

build_one src/user/rwnfs_probe.c rwnfs_probe || { echo "BUILD FAILED (probe)"; exit 1; }
llvm-objcopy -O binary "$OUT/rwnfs_probe.elf" "$OUT/rwnfs_probe.bin"

# The exec fixture is the tree's native dynamic pilot, copied to the export
# under an 8.3 name: the question is whether the ELF loader can open an image
# that lives on NFS.
if [ "${RW_SKIP_EXPORT:-0}" != "1" ]; then
  bash tools/rw/nfs-export.sh sync >"$OUT/export.log" 2>&1 || {
    echo "EXPORT FAILED"; tail -30 "$OUT/export.log"; exit 1; }
  tail -4 "$OUT/export.log"
fi
if [ ! -d /srv/nfs/rimworld ]; then echo "ENVIRONMENT: /srv/nfs/rimworld missing"; exit 2; fi
HDYN="$ROOT/obj/intel/HDYN.BIN"
[ -f "$HDYN" ] || { echo "ENVIRONMENT: obj/intel/HDYN.BIN missing (make ARCH=intel obj/intel/HDYN.BIN)"; exit 2; }
cp -f "$HDYN" /srv/nfs/rimworld/hdyn.bin
ls -l /srv/nfs/rimworld/hdyn.bin

# Fixture for the FAT16 LFN half: a long mixed-case name written by mcopy
# (i.e. a real VFAT long-name record + a short alias), which the probe opens
# by the long name.
LFN_SRC="$OUT/lfn_fixture"
printf 'LFN-OK:RimWorldLinux_Data:0001\n' > "$LFN_SRC"

CFG="$OUT/wave.cfg"
printf 'RWNFS.BIN\n' > "$CFG"

STAGE="mcopy -o -i $ROOT/disk.img $OUT/rwnfs_probe.bin ::/RWNFS.BIN && \
mcopy -o -i $ROOT/disk.img $LFN_SRC ::/RimWorldLinux_Data && \
mcopy -o -i $ROOT/disk.img $CFG ::/WAVE.CFG"

echo "== run: t3_tier intel (deadline ${TIMEOUT_S}s, log $LOG)"
T3_TIMEOUT="$TIMEOUT_S" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP=1 \
  T3_LOG="$LOG" bash tools/t3_tier.sh intel >"$OUT/tier.log" 2>&1
TIER_RC=$?
echo "t3_tier rc=$TIER_RC"

echo
echo "== guest serial, [RW] lines"
grep -a '\[RW\]' "$LOG" 2>/dev/null || echo "(no [RW] lines — the probe never ran; see $OUT/tier.log)"

echo
echo "== verdict"
fail=0
grep -aq '\[RW\] done failures=0' "$LOG" || { echo "FAIL: no 'done failures=0'"; fail=1; }
grep -aq '\[RW\] PASS mount the RimWorld export' "$LOG" || { echo "FAIL: mount"; fail=1; }
grep -aq '\[RW\] SUM /NFS/UnityPlayer.so bytes=65536' "$LOG" || { echo "FAIL: 64 KiB read"; fail=1; }
if grep -aq 'HDYN: done' "$LOG"; then
  echo "exec-from-NFS: OK (HDYN.BIN ran from /NFS: 'HDYN: done' on the console)"
else
  echo "exec-from-NFS: GAP (kernel opened the NFS path with fat16_open — see console)"
fi
grep -a '\[RW\] FAIL' "$LOG" || true
[ "$fail" = 0 ] && echo "rwnfs_probe: PASS" || echo "rwnfs_probe: FAIL"
exit "$fail"
