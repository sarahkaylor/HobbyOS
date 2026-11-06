#!/usr/bin/env bash
# tools/rw/run-nfs-bench.sh — build and run src/user/rwnfs_bench.c in the
# intel guest against the live RimWorld NFS export, and print its [BENCH] rows.
#
# This is the throughput gate for the NFS data path: it reads
# resources.assets (9.9 MB) with 1/4/16/64 KiB read()s and a 64 MiB span, and
# each row carries bytes, ms and the kernel's READ-RPC count so MB/s can be
# recomputed by hand.  Run it in-guest — a host `dd` measures nfsd, not the
# guest's transport.
#
# Usage: tools/rw/run-nfs-bench.sh [--timeout SECS]
# Env:   RW_NFS_SERVER (default 10.0.2.2), RW_NFS_ALT (default 192.168.0.123),
#        RW_SKIP_EXPORT=1 to skip the export sync.
# Exit:  0 when every phase reported its full byte count, 1 otherwise.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/rw/lane.sh
. "$ROOT/tools/rw/lane.sh"
cd "$ROOT" || exit 2
TIMEOUT_S=420
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S="$2"; shift 2 ;;
    *) echo "usage: $0 [--timeout SECS]" >&2; exit 2 ;;
  esac
done

SERVER="${RW_NFS_SERVER:-10.0.2.2}"
ALT="${RW_NFS_ALT:-192.168.0.123}"
OUT="$ROOT/obj/nfs-bench"
LOG="$OUT/serial.log"
mkdir -p "$OUT"

[ -f src/user/rwnfs_bench.c ] || { echo "missing src/user/rwnfs_bench.c"; exit 2; }

if [ ! -f "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" ] && \
   [ -f "/home/$(id -un)/.local/share/OVMF/OVMF_CODE_4M.fd" ]; then
  HOME="/home/$(id -un)"; export HOME
fi
PATH="/usr/sbin:/sbin:$PATH"; export PATH
for t in clang ld.lld llvm-objcopy mcopy mkfs.fat qemu-system-x86_64; do
  command -v "$t" >/dev/null 2>&1 || { echo "ENVIRONMENT: missing $t"; exit 2; }
done

echo "== build: kernel (MODE=test) + libc.a"
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=test obj/intel/libc.a hobbyos.elf disk.img \
  >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -25 "$OUT/build.log"; exit 1; }

RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large
         -DRW_NFS_SERVER="\"$SERVER\"" -DRW_NFS_ALT="\"$ALT\""
         -DRW_BENCH_BYTES="${RW_BENCH_BYTES:-0}")
echo "== build: src/user/rwnfs_bench.c"
clang "${UCFLAGS[@]}" -c src/user/rwnfs_bench.c -o "$OUT/rwnfs_bench.o" || exit 1
ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwnfs_bench.elf" \
       "$OUT/rwnfs_bench.o" obj/intel/libc.a || exit 1
llvm-objcopy -O binary "$OUT/rwnfs_bench.elf" "$OUT/rwnfs_bench.bin"

if [ "${RW_SKIP_EXPORT:-0}" != 1 ]; then
  bash tools/rw/nfs-export.sh sync >"$OUT/export.log" 2>&1 || {
    echo "EXPORT FAILED"; tail -20 "$OUT/export.log"; exit 1; }
  tail -3 "$OUT/export.log"
fi
[ -d "$RW_EXPORT" ] || { echo "ENVIRONMENT: $RW_EXPORT missing"; exit 2; }

CFG="$OUT/wave.cfg"
printf 'RWBENCH.BIN\n' >"$CFG"
printf '%s\n' "$RW_EXPORT" >"$OUT/nfsexport"
STAGE="mcopy -o -i $ROOT/disk.img $OUT/rwnfs_bench.bin ::/RWBENCH.BIN && mcopy -o -i $ROOT/disk.img $OUT/nfsexport ::/NFSEXPORT && mcopy -o -i $ROOT/disk.img $CFG ::/WAVE.CFG"

echo "== run: t3_tier intel (deadline ${TIMEOUT_S}s, log $LOG)"
T3_TIMEOUT="$TIMEOUT_S" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP=1 \
  T3_LOG="$LOG" bash tools/t3_tier.sh intel >"$OUT/tier.log" 2>&1
echo "t3_tier rc=$?"

echo
echo "== guest serial, [BENCH] rows"
grep -a '\[BENCH\]' "$LOG" 2>/dev/null || echo "(no [BENCH] rows — see $OUT/tier.log)"

echo
echo "== kernel RPC latency table ([NFSRTT])"
grep -a '\[NFSRTT\]' "$LOG" 2>/dev/null | tail -20 || true
echo
echo "== [NFSIO] progress rows"
grep -a '\[NFSIO\]' "$LOG" 2>/dev/null | tail -6 || true

fail=0
grep -aq '\[BENCH\] done failures=0' "$LOG" || { echo "FAIL: no 'done failures=0'"; fail=1; }
if grep -a '\[BENCH\] FAIL' "$LOG"; then fail=1; fi
[ "$fail" = 0 ] && echo "nfs_bench: PASS" || echo "nfs_bench: FAIL"
exit "$fail"
