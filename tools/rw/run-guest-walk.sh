#!/usr/bin/env bash
# tools/rw/run-guest-walk.sh — L58: what the guest actually enumerates.
#
# Builds src/user/rwwalk.c, stages it as /GAMES/RWWALK.BIN, boots the desktop
# harness and runs it.  The probe walks /NFS/Data completely (every directory,
# every file, stat() size) over getdents64, then re-lists
# Core/Defs/SoundDefs at four buffer sizes.
#
# ORDER MATTERS: `make disk.img` REGENERATES the FAT image, so the probe must
# be copied in AFTER the build and the boot must not rebuild the image
# (`make -o disk.img`).  A kernel change needs WALK_FULL_BUILD=1.
#
# Output: [W] rows in obj/rw-desktop/serial-desktop.log, summary in
# obj/rw-desktop/walk.out, and a host-side diff in obj/rw-desktop/walk-diff.txt.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/rw/lane.sh
. "$ROOT/tools/rw/lane.sh"
cd "$ROOT" || exit 2
export HOME=/home/sarah
OUT="$ROOT/obj/rw-desktop"
IMG="$ROOT/disk.img"
GAME="$RW_EXPORT"
[ -d "$GAME/Data" ] || { echo "no game tree at $GAME"; exit 2; }
mkdir -p "$OUT"

RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/include -Isrc/libc/include \
      --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc \
      -isystem "$RESINC" -mcmodel=large -c src/user/rwwalk.c -o obj/rwwalk.o || exit 2
ld.lld -T src/user/linker.ld -e _start -o obj/rwwalk.elf obj/rwwalk.o obj/intel/libc.a || exit 2
llvm-objcopy -O binary obj/rwwalk.elf obj/rwwalk.bin || exit 2

if [ "${WALK_FULL_BUILD:-0}" = 1 ] || [ ! -f "$IMG" ]; then
  echo "== build kernel + disk (this re-creates disk.img)"
  TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP=hobby \
      hobbyos.elf disk.img >"$OUT/walk-build.log" 2>&1 || {
    echo "build failed — see $OUT/walk-build.log"; exit 2; }
fi

mmd -i "$IMG" ::/GAMES >/dev/null 2>&1 || true
mcopy -o -i "$IMG" obj/rwwalk.bin ::/GAMES/RWWALK.BIN || exit 2
echo "staged ::/GAMES/RWWALK.BIN ($(stat -c %s obj/rwwalk.bin) bytes)"

echo "== run (RW_SKIP_BUILD=1, app RWWALK.BIN)"
RW_SKIP_BUILD=1 RW_APP=RWWALK.BIN bash tools/rw/run-rimworld-desktop.sh \
    --timeout 200 --run-timeout 150 >"$OUT/walk-run.out" 2>&1
S="$OUT/serial-desktop.log"
{
  echo "--- [W] summary rows ---"
  grep -aE '^\[W\] (S|TOT|BOUND|OPENFAIL|DENTSFAIL|BADREC|e |mount|begin|PROBE|FILE|NODIRFLAG|STAT)' "$S" | head -60
  echo "--- kernel rows ---"
  grep -aE '\[OPN\]|\[LNR\]|\[LNFSD\]|\[GDT\]' "$S" | head -30
} | tee "$OUT/walk.out"

# Host truth + diff, computed from the same tree the export serves.
python3 tools/rw/walk_diff.py "$GAME/Data" "$S" "$OUT/walk-diff.txt"
