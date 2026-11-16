#!/usr/bin/env bash
# tools/cp/nfs/nfs_bench.sh [tree] — cp/nfs guest NFS READ throughput
# (docs/cp-nfs.md).  Builds <tree> (default: this tree) and src/user/cpnfsbench.c
# FROM THIS TREE, exports a 64 MiB pseudo-random file read-only to 127.0.0.1
# ONLY (QEMU slirp reaches the host from localhost), boots the intel guest
# with the bench as the only wave member and prints its [NB] rows.  Run it on
# the base tree and the lane tree for before/after.
#
# Host hygiene: the export is ONE localhost-only read-only line added and
# removed with tools/cp/exports.sh (docs/cyberpunk.md §7.4: it holds
# /tmp/cp-exports.lock only while it edits /etc/exports, never across the
# boot, and checks every non-cp line is unchanged); the EXIT trap removes our
# line and the export directory.  The boot runs in a QEMU slot
# (tools/cp/qemu-slot.sh), so benches of different trees can run at once.
# Env: NB_EXPORT (default /srv/nfs/cp-nfs-bench-<tree dir name>), NB_TIMEOUT (default 420),
#      NB_BIG=1 (cp/nfs2, Gate P3's NFS half): also export a 5 GiB + 12345
#      byte file and let the guest SHA-256 it against the host's sha256sum.
#      It is written under $TREE/obj (strix: the root filesystem, never
#      /mnt/fast), renamed into the export dir, and removed with it.
set -uo pipefail
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TREE="$(cd "${1:-$SELF}" && pwd)"
EXPORT="${NB_EXPORT:-/srv/nfs/cp-nfs-bench-$(basename "$TREE" | tr -c 'A-Za-z0-9._\n-' _)}"
OUT="$TREE/obj/nfs-bench"; rm -rf "$OUT"; mkdir -p "$OUT"
die() { echo "nfs_bench: $*" >&2; exit 2; }
case "$EXPORT" in /srv/nfs/cp-nfs-bench*) ;; *) die "refusing export path $EXPORT";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists (another run?)"

echo "== build: tree $TREE (intel, test) + cpnfsbench.bin"
( cd "$TREE" && MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP=1 make ARCH=intel MODE=test hobbyos.elf disk.img \
    > "$OUT/build.log" 2>&1 && make -s ARCH=intel obj/intel/libc.a >> "$OUT/build.log" 2>&1 ) \
  || { tail -20 "$OUT/build.log"; die "build failed"; }
RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -I"$TREE/src/user_include" -I"$TREE/src/user_include/graphics" \
  -I"$TREE/src/include" -I"$TREE/src/libc/include" --target=x86_64-none-elf -ffreestanding \
  -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large -DNB_EXPORT="\"$EXPORT\"" \
  -c "$SELF/src/user/cpnfsbench.c" -o "$OUT/nb.o" || die "bench compile"
ld.lld -T "$TREE/src/user/linker.ld" -e _start -o "$OUT/nb.elf" "$OUT/nb.o" "$TREE/obj/intel/libc.a" || die "bench link"
llvm-objcopy -O binary "$OUT/nb.elf" "$OUT/nb.bin"
cc -O2 -o "$OUT/fnv64" "$SELF/tools/cp/nfs/fnv64.c" || die "fnv64"
"$OUT/fnv64" "$OUT/big64m.bin" $((64 * 1024 * 1024)) > "$OUT/expect.txt" || die "data"
echo "== data: 64 MiB, fnv1a64=$(cat "$OUT/expect.txt")"
# cp/nfs2: six 2 MiB files for the multi-file phase (seed k = the guest's
# multi_seed(k) in cpnfsbench.c)
for k in 0 1 2 3 4 5; do
  seed=$(python3 -I -c "print(hex(0x9e3779b97f4a7c15 ^ (($k + 1) * 0xbf58476d1ce4e5b9 % 2**64)))")
  "$OUT/fnv64" "$OUT/m$k.bin" $((2 * 1024 * 1024)) "$seed" > /dev/null || die "data m$k"
done
BIG=""
if [ "${NB_BIG:-0}" = 1 ]; then
  BIG_BYTES=$((5 * 1024 * 1024 * 1024 + 12345))
  case "$(df -P "$OUT" | awk 'NR==2{print $6}')" in /mnt/fast*) die "refusing to write 5 GiB under /mnt/fast";; esac
  "$OUT/fnv64" "$OUT/big5g.bin" $BIG_BYTES 0x2545f4914f6cdd1d > /dev/null || die "data big5g"
  echo "$BIG_BYTES $(sha256sum "$OUT/big5g.bin" | cut -c1-64)" > "$OUT/big.txt"
  echo "== data: big5g.bin $(cat "$OUT/big.txt")"
  BIG="$OUT/big5g.bin"
fi
printf 'CPNFSB.BIN\n' > "$OUT/wave.cfg"
STAGE="mcopy -o -i $TREE/disk.img $OUT/nb.bin ::/CPNFSB.BIN && mcopy -o -i $TREE/disk.img $OUT/expect.txt ::/NBEXPECT.TXT && mcopy -o -i $TREE/disk.img $OUT/wave.cfg ::/WAVE.CFG"
[ -n "$BIG" ] && STAGE="$STAGE && mcopy -o -i $TREE/disk.img $OUT/big.txt ::/NBBIG.TXT"

export TREE OUT EXPORT STAGE BIG NB_TIMEOUT="${NB_TIMEOUT:-420}"
# The export dir and its files first, then the export (tools/cp/exports.sh
# holds /tmp/cp-exports.lock only while it edits /etc/exports), then the boot
# in a QEMU slot.  The EXIT trap removes exactly our line and the directory.
ADDED=0
cleanup() {
  trap - EXIT INT TERM HUP
  [ "$ADDED" = 1 ] && bash "$SELF/tools/cp/exports.sh" remove "$EXPORT"
  sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  rm -f "$OUT/big5g.bin"   # if the run stopped before it moved into the export
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP
sudo -n mkdir -p "$EXPORT" && sudo -n cp "$OUT/big64m.bin" "$OUT"/m?.bin "$EXPORT/" || die "export dir"
if [ -n "$BIG" ]; then sudo -n mv "$BIG" "$EXPORT/" || die "big5g into the export"; fi
sudo -n chmod -R a+rX "$EXPORT" || die "chmod"
ADDED=1
bash "$SELF/tools/cp/exports.sh" add "$EXPORT" || die "exports.sh add"
( cd "$TREE" && T3_TIMEOUT="$NB_TIMEOUT" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP=1 \
    bash "$SELF/tools/cp/qemu-slot.sh" bash tools/t3_tier.sh intel > "$OUT/tier.log" 2>&1 )
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT/tier.log" 2>/dev/null | tail -1)"
[ -r "$CONSOLE" ] && cp "$CONSOLE" "$OUT/console.log"
grep -aE '^\[NB\]|\[NFS\] (fsinfo|mounted|rsize)|\[IPR\]' "$OUT/console.log" 2>/dev/null
grep -aE '^\[NFSPIPE\] len' "$OUT/console.log" 2>/dev/null | head -3
grep -aq '^\[NB\] done rc=0' "$OUT/console.log" 2>/dev/null && { echo "nfs_bench: PASS ($TREE)"; exit 0; }
echo "nfs_bench: FAIL ($TREE)"; exit 1
