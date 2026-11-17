#!/usr/bin/env bash
# tools/cp/pe/run-pemap.sh [tree] -- C1-pe: linux_probe_pemap against NFS- and
# ext2-backed files (docs/cp-pe.md).  Builds <tree> (default: this tree) and
# the probe + launcher FROM THIS TREE, writes the data files on the host with
# the probe's own generator, exports them read-only to 127.0.0.1 only
# (tools/cp/exports.sh; the EXIT trap removes the line and the directory),
# puts a second set on a fresh ext2 image (CPDATA, mounted at /EXT), and boots
# the intel guest in a QEMU slot with the launcher as the only wave member.
# Each boot runs the probe PE_RUNS times; the rows are graded per run.
#
# Env: PE_BOOTS (default 1)  boots, each a fresh guest
#      PE_RUNS (default 3)   probe runs per boot
#      PE_ARGS               probe arguments (default "-p 8 -c 32 -i 4 /CP /EXT/pe")
#      PE_TIMEOUT (default 900) seconds per boot
#      PE_EXPORT (default /srv/nfs/cp-pe-<tree dir name>)
# Prints one "pemap: boot b run r rc=..." row per run and a summary
# "pemap: SUMMARY runs=N pass=P fail=F" (exit 0 only when every run passed).
set -uo pipefail
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TREE="$(cd "${1:-$SELF}" && pwd)"
NAME="$(basename "$TREE" | tr -c 'A-Za-z0-9._\n-' _)"
EXPORT="${PE_EXPORT:-/srv/nfs/cp-pe-$NAME}"
BOOTS="${PE_BOOTS:-1}"
RUNS="${PE_RUNS:-3}"
ARGS="${PE_ARGS:--p 8 -c 32 -i 4 /CP /EXT/pe}"
OUT="$TREE/obj/pemap"; mkdir -p "$OUT"
die() { echo "pemap: $*" >&2; exit 2; }
case "$EXPORT" in /srv/nfs/cp-pe*) ;; *) die "refusing export path $EXPORT";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists (another run?)"
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP=1 LO_ON_DISK="${LO_ON_DISK-}"

echo "== build: tree $TREE (intel, test), probe + launcher from $SELF"
cp "$SELF/src/user/linux_probe_pemap.c" "$TREE/src/user/linux_probe_pemap.c" 2>/dev/null || true
( cd "$TREE" && make ARCH=intel MODE=test hobbyos.elf disk.img > "$OUT/build.log" 2>&1 &&
  make -s ARCH=intel obj/intel/libc.a obj/intel/probe_pemap.elf >> "$OUT/build.log" 2>&1 ) ||
  { tail -20 "$OUT/build.log"; die "build failed"; }
RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -I"$TREE/src/user_include" -I"$TREE/src/user_include/graphics" \
  -I"$TREE/src/include" -I"$TREE/src/libc/include" --target=x86_64-none-elf -ffreestanding \
  -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large -DPE_EXPORT="\"$EXPORT\"" \
  -DPE_ARGS="\"$ARGS\"" -DPE_RUNS="$RUNS" -c "$SELF/tools/cp/pe/pemapl.c" -o "$OUT/pl.o" || die "launcher compile"
ld.lld -T "$TREE/src/user/linker.ld" -e _start -o "$OUT/pl.elf" "$OUT/pl.o" "$TREE/obj/intel/libc.a" || die "launcher link"
llvm-objcopy -O binary "$OUT/pl.elf" "$OUT/pl.bin"
gcc -O2 -pthread -o "$OUT/pemap-host" "$SELF/src/user/linux_probe_pemap.c" 2>/dev/null || die "host probe"

echo "== data: NFS set + ext2 set (generator: the probe's --gen)"
rm -rf "$OUT/nfsdata" "$OUT/e2root" "$OUT/e2gen"; mkdir -p "$OUT/nfsdata" "$OUT/e2root/pe" "$OUT/e2gen"
"$OUT/pemap-host" --gen "$OUT/nfsdata" 5 1048576 > /dev/null || die "gen nfs"
# the ext2 set: seeds 6..8 (pemap05..07), so no file matches an NFS one
"$OUT/pemap-host" --gen "$OUT/e2gen" 8 786432 > /dev/null || die "gen ext2"
mv "$OUT"/e2gen/pemap0[5-7].bin "$OUT/e2root/pe/" && rm -rf "$OUT/e2gen" || die "ext2 set"
"$OUT/pemap-host" -s -p 8 -c 16 -i 3 "$OUT/nfsdata" "$OUT/e2root/pe" | tail -2 | sed 's/^/== host reference: /'
E2IMG="$OUT/cpdata-pe.img"
rm -f "$E2IMG"; truncate -s 512M "$E2IMG"
PATH="/usr/sbin:/sbin:$PATH" mke2fs -q -F -t ext2 -b 4096 -I 256 -O ^dir_index -m 0 -L cpdata \
  -E root_owner=0:0 -d "$OUT/e2root" "$E2IMG" || die "mke2fs"

ADDED=0
cleanup() {
  trap - EXIT INT TERM HUP
  [ "$ADDED" = 1 ] && bash "$SELF/tools/cp/exports.sh" remove "$EXPORT"
  sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP
sudo -n mkdir -p "$EXPORT" && sudo -n cp "$OUT"/nfsdata/pemap*.bin "$EXPORT/" && sudo -n chmod -R a+rX "$EXPORT" || die "export dir"
ADDED=1
bash "$SELF/tools/cp/exports.sh" add "$EXPORT" || die "exports.sh add"

PASS=0; FAIL=0; NR=0
for b in $(seq 1 "$BOOTS"); do
  printf 'PEMAPL.BIN\n' > "$OUT/wave.cfg"
  STAGE="mcopy -o -i $TREE/disk.img $OUT/pl.bin ::/PEMAPL.BIN && mcopy -o -i $TREE/disk.img $TREE/obj/intel/probe_pemap.elf ::/PEMAP.ELF && mcopy -o -i $TREE/disk.img $OUT/wave.cfg ::/WAVE.CFG"
  # a fresh ext2 image per boot (the probe only reads it, but /EXT must not
  # carry state from an earlier boot)
  cp --sparse=always "$E2IMG" "$OUT/cpdata-boot.img"
  ( cd "$TREE" && CPDATA="$OUT/cpdata-boot.img" T3_TIMEOUT="${PE_TIMEOUT:-900}" T3_STAGE="$STAGE" T3_MIN_LINES=1 \
      TOOLS_SKIP=1 bash "$SELF/tools/cp/qemu-slot.sh" bash tools/t3_tier.sh intel > "$OUT/tier-$b.log" 2>&1 )
  CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT/tier-$b.log" 2>/dev/null | tail -1)"
  [ -r "$CONSOLE" ] && cp "$CONSOLE" "$OUT/console-$b.log"
  C="$OUT/console-$b.log"
  grep -aE '^\[(LP|PE)\]|\[EXT2\] mount|TLBTIMEOUT|\[KERNEL\].*fault' "$C" 2>/dev/null | head -80 | sed "s/^/[boot $b] /"
  r=0
  while IFS= read -r line; do
    r=$((r + 1)); NR=$((NR + 1))
    case "$line" in *"rc=0"*) PASS=$((PASS + 1)); echo "pemap: boot $b run $r rc=0";;
                    *) FAIL=$((FAIL + 1)); echo "pemap: boot $b run $r ${line#\[LP\] done }";; esac
  done < <(grep -a '^\[LP\] done rc=' "$C" 2>/dev/null)
  if [ "$r" -lt "$RUNS" ]; then
    FAIL=$((FAIL + RUNS - r)); NR=$((NR + RUNS - r))
    echo "pemap: boot $b: only $r of $RUNS runs finished"
  fi
done
echo "pemap: SUMMARY tree=$TREE runs=$NR pass=$PASS fail=$FAIL"
[ "$FAIL" = 0 ] && [ "$NR" -gt 0 ]
