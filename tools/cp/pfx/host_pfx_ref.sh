#!/usr/bin/env bash
# tools/cp/pfx/host_pfx_ref.sh - the Linux-host reference for the prefix-on-ext2
# two-boot test (lane C3-pfx, docs/cp-pfx.md): the same staged Wine and the
# same glibc 2.31, a prefix on a fresh tools/cp/mkcpdata.sh image mounted by
# LINUX's ext2 driver, two separate "boots" (two fresh mount namespaces, each
# with its own wineserver):
#   run 1: wineboot -i                 then wineserver -w (wait for it to exit)
#   run 2: wine cmd /c echo hello      then wineserver -w
# A manifest of the prefix (type, mtime, size, path) is taken after run 1,
# before run 2 and after run 2, so the files a second start legitimately
# rewrites (the registry hives) are measured, not guessed.  e2fsck -fn at the
# end.  Nothing is installed or changed outside the scratch dir: the mounts
# live in private mount namespaces, Wine runs as the calling user in a chroot
# that holds only the 2.31 runtime and the staged Wine (bind-mounted ro).
#
# Usage: bash tools/cp/pfx/host_pfx_ref.sh OUTDIR
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE),
#      GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu),
#      WINEDEBUG (default err+all,fixme-all,+wineboot)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:?usage: host_pfx_ref.sh OUTDIR}"
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
WDBG="${WINEDEBUG:-err+all,fixme-all,+wineboot}"
PATH="/usr/sbin:/sbin:$PATH"
uid=$(id -u) gid=$(id -g)
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RF="$OUT/root"
IMG="$OUT/cpdata.img"
rm -rf "$RF"
mkdir -p "$RF"/{tmp,dev,proc,etc,EXT,CP/WINE,lib64,lib/x86_64-linux-gnu}
chmod 1777 "$RF/tmp"
printf 'cp:x:%s:%s:cp:/tmp:/bin/false\n' "$uid" "$gid" > "$RF/etc/passwd"
printf 'cp:x:%s:\n' "$gid" > "$RF/etc/group"
CPDATA_FORCE=1 bash "$ROOT/tools/cp/mkcpdata.sh" "$IMG" 8G

# run <tag> <prog> <args...>: one "boot" = one private namespace
run() {
  local tag="$1"; shift
  local inner='R=$1; IMG=$2; U=$3; G=$4; S=$5; L=$6; D=$7; shift 7
mount --rbind /dev "$R/dev"; mount -t proc proc "$R/proc"
mount --bind "$S" "$R/CP/WINE"; mount -o remount,bind,ro "$R/CP/WINE"
mount --bind "$L" "$R/lib/x86_64-linux-gnu"; mount -o remount,bind,ro "$R/lib/x86_64-linux-gnu"
cp -L "$L/ld-linux-x86-64.so.2" "$R/lib64/ld-linux-x86-64.so.2"
mount -t ext2 -o loop "$IMG" "$R/EXT"
chown "$U:$G" "$R/EXT" # Wine creates the prefix itself; it checks the parent owner
man() { (cd "$R/EXT" && find pfx -printf "%y %T@ %s %p\n" | LC_ALL=C sort -k4); }
[ -d "$R/EXT/pfx" ] && man > "$D/manifest-$TAG-before.txt"
E="env -i HOME=/tmp USER=cp WINEPREFIX=/EXT/pfx WINEDEBUG=$W PATH=/CP/WINE/bin LANG=C"
C=$(command -v chroot)
$E "$C" --userspec="$U:$G" "$R" "$@"; rc=$?
$E "$C" --userspec="$U:$G" "$R" /CP/WINE/bin/wineserver -w
man > "$D/manifest-$TAG-after.txt"
umount "$R/EXT"; exit $rc'
  echo "== host ref: $tag: $*"
  set +e
  sudo TAG="$tag" W="$WDBG" unshare -m --propagation private sh -c "$inner" sh \
    "$RF" "$IMG" "$uid" "$gid" "$STAGE" "$LDIR" "$OUT" "$@" > "$OUT/$tag.log" 2>&1
  local rc=$?
  set -e
  echo "   exit $rc; $(wc -l < "$OUT/$tag.log") log lines"
  sudo chown "$uid:$gid" "$OUT"/manifest-* 2>/dev/null || true
}
run boot1 /CP/WINE/bin/wineboot -i
run boot2 /CP/WINE/bin/wine cmd /c echo hello
e2fsck -fn "$IMG" > "$OUT/fsck.log" 2>&1 && echo "== e2fsck -fn rc=0: $(tail -1 "$OUT/fsck.log")" || { echo "== e2fsck FAIL"; cat "$OUT/fsck.log"; }
echo "== boot2 output:"; grep -av '^[0-9a-f]\{4\}:trace:wineboot' "$OUT/boot2.log" | head -20
echo "== wineboot update rows: boot1 $(grep -ac 'has been updated' "$OUT/boot1.log"), boot2 $(grep -ac 'has been updated' "$OUT/boot2.log")"
echo "== created-config rows: boot1 $(grep -ac 'created the configuration directory' "$OUT/boot1.log"), boot2 $(grep -ac 'created the configuration directory' "$OUT/boot2.log")"
echo "== persisted: boot1-after vs boot2-before: $(diff -q "$OUT/manifest-boot1-after.txt" "$OUT/manifest-boot2-before.txt" >/dev/null && echo identical || echo DIFFER)"
echo "== changed by boot 2 (mtime or size; + new, - gone):"
diff <(awk '{print $4, $2, $3}' "$OUT/manifest-boot2-before.txt") <(awk '{print $4, $2, $3}' "$OUT/manifest-boot2-after.txt") | grep '^[<>]' | sed 's/^</-/; s/^>/+/' | head -40
echo "== entries: $(wc -l < "$OUT/manifest-boot2-after.txt")"
rm -f "$IMG"
