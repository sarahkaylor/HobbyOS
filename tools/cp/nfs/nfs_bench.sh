#!/usr/bin/env bash
# tools/cp/nfs/nfs_bench.sh [tree] — cp/nfs guest NFS READ throughput
# (docs/cp-nfs.md).  Builds <tree> (default: this tree) and src/user/cpnfsbench.c
# FROM THIS TREE, exports a 64 MiB pseudo-random file read-only to 127.0.0.1
# ONLY (QEMU slirp reaches the host from localhost), boots the intel guest
# with the bench as the only wave member and prints its [NB] rows.  Run it on
# the base tree and the lane tree for before/after.
#
# Host hygiene (same pattern as tools/cp/run-wine-smoke.sh): /etc/exports is
# backed up and, on every exit path, restored BYTE-FOR-BYTE when the only
# change is our one line; if another harness edited it meanwhile, only our own
# line is removed (never their export).  The export directory is removed.  The
# whole export lifetime and the boot run under flock /tmp/cp-exports.lock
# (outer: every harness that edits /etc/exports should hold it from backup to
# restore -- a whole-file restore by a harness that snapshotted the file while
# our line was in it puts our line back, measured 2026-11-15) and then
# /tmp/cp-qemu.lock.
# Env: NB_EXPORT (default /srv/nfs/cp-nfs-bench), NB_TIMEOUT (default 420).
set -uo pipefail
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TREE="$(cd "${1:-$SELF}" && pwd)"
EXPORT="${NB_EXPORT:-/srv/nfs/cp-nfs-bench}"
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
printf 'CPNFSB.BIN\n' > "$OUT/wave.cfg"
STAGE="mcopy -o -i $TREE/disk.img $OUT/nb.bin ::/CPNFSB.BIN && mcopy -o -i $TREE/disk.img $OUT/expect.txt ::/NBEXPECT.TXT && mcopy -o -i $TREE/disk.img $OUT/wave.cfg ::/WAVE.CFG"

LINE="$EXPORT 127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)"
export TREE OUT EXPORT LINE STAGE NB_TIMEOUT="${NB_TIMEOUT:-420}"
flock /tmp/cp-exports.lock flock /tmp/cp-qemu.lock bash -c '
  set -u
  BAK="$OUT/exports.backup"
  sudo -n cp -a /etc/exports "$BAK" || exit 3
  cleanup() {
    if sudo -n test -f "$BAK"; then
      if printf "%s\n" "$LINE" | sudo -n sh -c "cat \"$BAK\" - | cmp -s - /etc/exports"; then
        sudo -n cp -a "$BAK" /etc/exports
      else
        echo "== /etc/exports changed by someone else meanwhile: removing only our line"
        sudo -n python3 -I -c "import sys;p=\"/etc/exports\";l=sys.argv[1]+\"\\n\";s=open(p).read();open(p,\"w\").write(s.replace(l,\"\",1))" "$LINE"
      fi
      sudo -n exportfs -ra
      if sudo -n cmp -s "$BAK" /etc/exports; then echo "== /etc/exports restored (cmp identical to backup)"
      else echo "== NOTE: /etc/exports differs from our backup (another harness edited it); our line is gone: $(grep -cF "$EXPORT " /etc/exports)"; fi
    fi
    sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  }
  trap cleanup EXIT
  sudo -n mkdir -p "$EXPORT" && sudo -n cp "$OUT/big64m.bin" "$OUT"/m?.bin "$EXPORT/" && sudo -n chmod -R a+rX "$EXPORT" || exit 3
  printf "%s\n" "$LINE" | sudo -n tee -a /etc/exports > /dev/null
  sudo -n exportfs -ra
  cd "$TREE" && T3_TIMEOUT="$NB_TIMEOUT" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP=1 \
    bash tools/t3_tier.sh intel > "$OUT/tier.log" 2>&1
  true
'
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT/tier.log" 2>/dev/null | tail -1)"
[ -r "$CONSOLE" ] && cp "$CONSOLE" "$OUT/console.log"
grep -aE '^\[NB\]|\[NFS\] (fsinfo|mounted|rsize)|\[IPR\]' "$OUT/console.log" 2>/dev/null
grep -aE '^\[NFSPIPE\] len' "$OUT/console.log" 2>/dev/null | head -3
grep -aq '^\[NB\] done rc=0' "$OUT/console.log" 2>/dev/null && { echo "nfs_bench: PASS ($TREE)"; exit 0; }
echo "nfs_bench: FAIL ($TREE)"; exit 1
