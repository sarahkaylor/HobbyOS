#!/usr/bin/env bash
# tools/cp/nfs/nfs_bench.sh [tree] — cp/nfs guest NFS READ throughput
# (docs/cp-nfs.md).  Builds <tree> (default: this tree) and src/user/cpnfsbench.c
# FROM THIS TREE, exports a 64 MiB pseudo-random file read-only to 127.0.0.1
# ONLY (QEMU slirp reaches the host from localhost), boots the intel guest
# with the bench as the only wave member and prints its [NB] rows.  Run it on
# the base tree and the lane tree for before/after.
#
# Host hygiene: the export is ONE localhost-only read-only line added and
# removed with tools/cp/exports.sh (docs/cyberpunk.md §7.4), which holds
# /tmp/cp-exports.lock only while it edits /etc/exports and checks that every
# non-cp line is unchanged; the EXIT trap removes our line and the export
# directory.  The boot runs in a QEMU slot (tools/cp/qemu-slot.sh), and the
# default export name carries the tree's directory name, so the benches of
# different trees can run at once.
# Env: NB_EXPORT (default /srv/nfs/cp-nfs-bench-<tree dir name>), NB_TIMEOUT (default 420).
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
printf 'CPNFSB.BIN\n' > "$OUT/wave.cfg"
STAGE="mcopy -o -i $TREE/disk.img $OUT/nb.bin ::/CPNFSB.BIN && mcopy -o -i $TREE/disk.img $OUT/expect.txt ::/NBEXPECT.TXT && mcopy -o -i $TREE/disk.img $OUT/wave.cfg ::/WAVE.CFG"

export TREE OUT EXPORT STAGE NB_TIMEOUT="${NB_TIMEOUT:-420}"
# docs/cyberpunk.md §7.4: the export is added and removed with
# tools/cp/exports.sh (it holds /tmp/cp-exports.lock only while it edits the
# file, never across the boot); the boot runs in a QEMU slot.
ADDED=0
cleanup() {
  trap - EXIT INT TERM HUP
  [ "$ADDED" = 1 ] && bash "$SELF/tools/cp/exports.sh" remove "$EXPORT"
  sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP
sudo -n mkdir -p "$EXPORT" && sudo -n cp "$OUT/big64m.bin" "$EXPORT/" || die "export dir"
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
