#!/usr/bin/env bash
# tools/cp/ext2_gate.sh — the cp/ext2 in-OS gate (docs/cp-ext2.md).
#
#   1. make a fresh ext2 data disk with tools/cp/mkcpdata.sh
#   2. boot 1 (x86_64, test wave = linux_probe_ext2 only, disk attached via
#      CPDATA): the probe builds its tree under /EXT/lp (phase 1)
#   3. host: e2fsck -fn must exit 0; debugfs must show exactly the expected
#      tree; every regular file's bytes (sha256) must equal ext2_expect.py's
#   4. boot 2: the probe finds the phase-1 marker and re-reads everything
#      (persistence across a reboot), then appends one record (phase 2)
#   5. host: e2fsck -fn again; the phase-2 bytes
#   6. cp/ext2b: build two glibc 2.31 programs (static-pie, and dynamic with
#      PT_INTERP/RUNPATH /EXT/lpx/glibc), store them, ld.so and libc.so.6 on the
#      image with debugfs; boot 3 runs linux_probe_ext2b (main + a "helper"
#      wave member for the two-process lock rows) and both glibc programs
#      FROM the ext2 volume
#   7. host: e2fsck -fn; debugfs: the modes/owners/times the probe set, the
#      mapped-store bytes (m.bin == m.expect), the exec markers
#
# Every QEMU boot runs under `flock /tmp/cp-qemu.lock` (shared CI box).
# Env: GATE_DIR (default <repo>/obj/ext2-gate), CPDATA_SIZE (default 8G),
#      GATE_KEEP=1 keeps the image, GLIBC_SYSROOT (default
#      ~/cp/wine-build/sysroot-glibc231; tools/cp/build-wine.sh makes it),
#      GATE_ONLY_EXT2B=1 skips boots 1-2.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
D="${GATE_DIR:-$ROOT/obj/ext2-gate}"  # images stay under the tree's obj/
IMG="$D/cpdata.img"
PATH="/usr/sbin:/sbin:$PATH"
fails=0
say() { printf '%s\n' "$*"; }
bad() { say "[HOST] FAIL $*"; fails=$((fails + 1)); }
good() { say "[HOST] ok   $*"; }

rm -rf "$D" && mkdir -p "$D"
CPDATA_FORCE=1 bash "$ROOT/tools/cp/mkcpdata.sh" "$IMG" "${CPDATA_SIZE:-8G}" || exit 2

boot() { # boot <n>
  local n="$1"
  say "=== ext2_gate: boot $n (CPDATA=$IMG) ==="
  CPDATA="$IMG" LP_OUT="$D/boot$n.tier.log" LP_TIMEOUT="${LP_TIMEOUT:-600}" \
    flock /tmp/cp-qemu.lock bash "$ROOT/tools/gate/t3_linux_probe.sh" ext2 \
    > "$D/boot$n.gate.log" 2>&1
  local rc=$?
  local console
  console="$(sed -n 's/^T3_LOG=//p' "$D/boot$n.tier.log" | tail -1)"
  [ -n "$console" ] && cp "$console" "$D/boot$n.console.log"
  grep -a -E '^\[(BLKD|EXT2)\]' "$D/boot$n.console.log" 2>/dev/null | head -3
  grep -a '^\[LP\]' "$D/boot$n.console.log" 2>/dev/null | grep -v '^\[LP\] ok  ' || true
  say "boot $n: $(grep -ac '^\[LP\] ok  ' "$D/boot$n.console.log" 2>/dev/null) ok rows; $(tail -1 "$D/boot$n.gate.log")"
  grep -aq '^\[EXT2\] mounted /EXT' "$D/boot$n.console.log" || bad "boot $n: no [EXT2] mounted row"
  [ "$rc" = 0 ] || bad "boot $n: probe gate rc=$rc (see $D/boot$n.gate.log)"
}

dbg() { debugfs -R "$1" "$IMG" 2>/dev/null; }

check_image() { # check_image <phase>
  local phase="$1" exp="$D/expect$1" w="$D/work$1"
  rm -rf "$exp" "$w" && mkdir -p "$w"
  python3 "$ROOT/tools/cp/ext2_expect.py" "$exp" "$phase" || { bad "ext2_expect.py"; return; }
  say "--- host: e2fsck -fn $IMG (after boot $phase) ---"
  e2fsck -fn "$IMG" > "$D/fsck$phase.log" 2>&1
  local frc=$?
  cat "$D/fsck$phase.log"
  [ "$frc" = 0 ] && good "e2fsck -fn rc=0" || bad "e2fsck -fn rc=$frc"
  say "--- host: debugfs view of /lp ---"
  dbg "ls -l /lp" | sed -n '1,40p'
  # exact name set of /lp
  local got want
  got="$(dbg "ls -p /lp" | awk -F/ 'NF>5 && $6!="." && $6!=".." {print $6}' | LC_ALL=C sort | tr '\n' ' ')"
  want="big.bin dosdevices hello.hard hello.txt link_dir link_fast link_slow phase1.done "
  [ "$phase" = 2 ] && want="${want}phase2.done "
  want="${want}sparse5g sub sub2 trunc.bin trunc2.bin victim.txt "
  [ "$got" = "$want" ] && good "/lp names = [$got]" || bad "/lp names = [$got] want [$want]"
  while read -r kind name arg; do
    case "$kind" in
    f)
      dbg "dump /$name $w/got" >/dev/null
      local hs es
      hs="$(sha256sum < "$w/got" | cut -c1-64)"
      es="$(sha256sum < "$exp/$arg" | cut -c1-64)"
      if [ "$hs" = "$es" ]; then good "$name sha256 $hs ($(stat -c %s "$w/got") bytes)"
      else bad "$name sha256 $hs want $es"; fi
      ;;
    d)
      dbg "stat /$name" | grep -q 'Type: directory' && good "$name is a directory" || bad "$name not a directory"
      ;;
    l)
      rm -rf "$w/rd" && mkdir -p "$w/rd"
      dbg "rdump /$name $w/rd" >/dev/null
      local t
      t="$(readlink "$w/rd/$(basename "$name")" 2>/dev/null)"
      [ "$t" = "$arg" ] && good "$name -> $t" || bad "$name -> '$t' want '$arg'"
      ;;
    x)
      dbg "stat /$name" 2>&1 | grep -q 'Inode:' && bad "$name still exists" || good "$name absent"
      ;;
    esac
  done < "$exp/MANIFEST"
  # hard link: one inode, two names, link count 2
  local i1 i2 lc
  i1="$(dbg "ls -p /lp" | awk -F/ '$6=="hello.txt"{print $2}')"
  i2="$(dbg "ls -p /lp" | awk -F/ '$6=="hello.hard"{print $2}')"
  lc="$(dbg "stat /lp/hello.txt" | sed -n 's/.*Links: \([0-9]*\).*/\1/p' | head -1)"
  [ -n "$i1" ] && [ "$i1" = "$i2" ] && [ "$lc" = 2 ] && good "hello.hard = inode $i1, Links: $lc" || bad "hard link i1=$i1 i2=$i2 links=$lc"
  # sparse5g: size, the 5 GiB block via debugfs's own block map, a hole before it
  local sz pb hole data
  sz="$(dbg "stat /lp/sparse5g" | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)"
  [ "$sz" = 5368709135 ] && good "sparse5g Size: $sz" || bad "sparse5g Size: $sz"
  pb="$(dbg "bmap /lp/sparse5g 1310720" | tr -dc '0-9')"
  hole="$(dbg "bmap /lp/sparse5g 1000" | tr -dc '0-9')"
  data="$(dd if="$IMG" bs=4096 skip="${pb:-0}" count=1 status=none | head -c 15)"
  [ -n "$pb" ] && [ "$pb" != 0 ] && [ "$data" = "SPARSE-AT-5GiB" ] && good "sparse5g lblk 1310720 -> pblk $pb holds '$data'" || bad "sparse5g pblk=$pb data='$data'"
  [ "$hole" = 0 ] && good "sparse5g lblk 1000 is a hole" || bad "sparse5g lblk 1000 -> $hole (want hole)"
  local blocks
  blocks="$(dbg "stat /lp/sparse5g" | sed -n 's/.*Blockcount: \([0-9]*\).*/\1/p' | head -1)"
  say "[HOST] info sparse5g Blockcount: $blocks (512-byte units)"
}

if [ "${GATE_ONLY_EXT2B:-0}" != 1 ]; then
boot 1
check_image 1
boot 2
grep -aq '^\[LP\] phase2 complete' "$D/boot2.console.log" && good "boot 2 ran phase 2 (persistence)" || bad "boot 2 did not reach phase 2"
check_image 2
fi

# ---- cp/ext2b (docs/cp-ext2b.md) ------------------------------------------
SYSR="${GLIBC_SYSROOT:-$HOME/cp/wine-build/sysroot-glibc231}"
X2="$D/ext2b"
mkdir -p "$X2"
src="$ROOT/tools/cp/ext2b/exec_hello.c"
if clang --sysroot="$SYSR" -fuse-ld=lld -O1 -static-pie -fPIE -o "$X2/hs" "$src" &&
   clang --sysroot="$SYSR" -fuse-ld=lld -O1 -fPIE -pie \
     -Wl,--dynamic-linker=/EXT/lpx/glibc/ld-linux-x86-64.so.2 \
     -Wl,-rpath,/EXT/lpx/glibc -o "$X2/hd" "$src"; then
  good "built glibc 2.31 hs (static-pie) + hd (dynamic, PT_INTERP $(readelf -lW "$X2/hd" | sed -n 's/.*interpreter: \(.*\)]/\1/p'))"
else
  bad "glibc exec programs did not build (GLIBC_SYSROOT=$SYSR)"
fi
LDSO="$(readlink -f "$SYSR/lib64/ld-linux-x86-64.so.2")"
LIBC="$(readlink -f "$SYSR/lib/x86_64-linux-gnu/libc.so.6")"
{
  echo "mkdir lpx"; echo "mkdir lpx/bin"; echo "mkdir lpx/glibc"; echo "mkdir lpx/out"
  echo "write $X2/hs lpx/bin/hs"; echo "write $X2/hd lpx/bin/hd"
  echo "write $LDSO lpx/glibc/ld-linux-x86-64.so.2"; echo "write $LIBC lpx/glibc/libc.so.6"
  for f in lpx/bin/hs lpx/bin/hd lpx/glibc/ld-linux-x86-64.so.2 lpx/glibc/libc.so.6; do
    echo "sif $f mode 0100755"
  done
} > "$X2/stage.cmds"
debugfs -w -f "$X2/stage.cmds" "$IMG" > "$X2/stage.log" 2>&1
e2fsck -fn "$IMG" > "$D/fsck3pre.log" 2>&1 && good "staged lpx/{bin,glibc} with debugfs; e2fsck -fn rc=0" || { bad "e2fsck after staging"; cat "$D/fsck3pre.log"; }
for f in bin/hs bin/hd glibc/ld-linux-x86-64.so.2 glibc/libc.so.6; do
  say "[HOST] info staged /lpx/$f mode $(dbg "stat /lpx/$f" | sed -n 's/.*Mode: *\([0-7]*\).*/\1/p' | head -1) size $(dbg "stat /lpx/$f" | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)"
done

say "=== ext2_gate: boot 3 (linux_probe_ext2b + helper + glibc hs/hd from /EXT) ==="
CPDATA="$IMG" LP_OUT="$D/boot3.tier.log" LP_TIMEOUT="${LP_TIMEOUT:-600}" \
  LP_WAVE_EXTRA="$(printf 'PROBE.ELF /EXT helper\n/EXT/lpx/bin/hs static\n/EXT/lpx/bin/hd dyn')" \
  flock /tmp/cp-qemu.lock bash "$ROOT/tools/gate/t3_linux_probe.sh" ext2b \
  > "$D/boot3.gate.log" 2>&1
rc3=$?
console="$(sed -n 's/^T3_LOG=//p' "$D/boot3.tier.log" | tail -1)"
[ -n "$console" ] && cp "$console" "$D/boot3.console.log"
grep -a -E '^\[EXT2\]|elf loader: (interpreter|PT_INTERP)|\[E2MAP\]' "$D/boot3.console.log" 2>/dev/null | head -8
grep -a '\[LP\]' "$D/boot3.console.log" 2>/dev/null | grep -v '\[LP\] ok  \|\[LP\] helper ok  ' || true
say "boot 3: $(grep -ac '\[LP\] ok  ' "$D/boot3.console.log" 2>/dev/null) main ok rows, $(grep -ac '\[LP\] helper ok  ' "$D/boot3.console.log" 2>/dev/null) helper ok rows; $(tail -1 "$D/boot3.gate.log")"
[ "$rc3" = 0 ] || bad "boot 3: probe gate rc=$rc3 (see $D/boot3.gate.log)"

say "--- host: e2fsck -fn $IMG (after boot 3) ---"
e2fsck -fn "$IMG" > "$D/fsck3.log" 2>&1
frc=$?
cat "$D/fsck3.log"
[ "$frc" = 0 ] && good "e2fsck -fn rc=0 (after ext2b)" || bad "e2fsck -fn rc=$frc (after ext2b)"
field() { dbg "stat $1" | sed -n "s/.*$2 *\([0-9a-fx]*\).*/\1/p" | head -1; }
want_f() { # path label expected value(s)
  local got; got="$(field "$1" "$2")"
  [ "$got" = "$3" ] && good "$1 $2 $got" || bad "$1 $2 '$got' want '$3'"
}
say "--- host: debugfs stat /lpx/attr.f ---"
dbg "stat /lpx/attr.f" | sed -n '1,12p'
want_f /lpx/attr.f "Mode:" 0751
want_f /lpx/attr.f "User:" 1234
want_f /lpx/attr.f "Group:" 5678
want_f /lpx/attr.f "atime:" 0x3b9aca00
want_f /lpx/attr.f "mtime:" 0x499602d2
want_f /lpx/attr.l "User:" 4242
want_f /lpx/attr.l "Group:" 4444
want_f /lpx/attr.l "mtime:" 0x00000378
want_f /lpx/attr.d "Mode:" 02775
want_f /lpx/attr.d "User:" 7
want_f /lpx/attr.d "Group:" 8
dbg "dump /lpx/m.bin $X2/m.bin" >/dev/null; dbg "dump /lpx/m.expect $X2/m.expect" >/dev/null
hm="$(sha256sum < "$X2/m.bin" | cut -c1-64)"; he="$(sha256sum < "$X2/m.expect" | cut -c1-64)"
[ -s "$X2/m.bin" ] && [ "$hm" = "$he" ] && [ "$(stat -c %s "$X2/m.bin")" = 12388 ] \
  && good "m.bin sha256 $hm == m.expect (write()s + MAP_SHARED stores, 12388 bytes)" \
  || bad "m.bin sha256 $hm ($(stat -c %s "$X2/m.bin" 2>/dev/null) bytes) != m.expect $he"
grep -c "MAPPED-STORE" "$X2/m.bin" >/dev/null && good "m.bin holds the mapped stores ($(grep -ao 'MAPPED-STORE-[0-9]' "$X2/m.bin" | tr '\n' ' '))" || bad "no MAPPED-STORE in m.bin"
dbg "stat /lpx/unl.bin" 2>&1 | grep -q 'Inode:' && bad "unl.bin still linked" || good "unl.bin (unlinked while mapped) is gone; e2fsck saw no orphan"
dbg "dump /lpx/out/dyn.txt $X2/dyn.txt" >/dev/null
l="$(head -1 "$X2/dyn.txt" 2>/dev/null)"
case "$l" in "ok dyn "*"libcpath=/EXT/lpx/glibc/libc.so.6"*) good "/lpx/out/dyn.txt: $l" ;; *) bad "/lpx/out/dyn.txt: '$l'" ;; esac
# glibc static-pie (cp/exec, docs/cp-exec.md section 3): the loader defers its
# R_X86_64_IRELATIVE entries to the image's own _dl_relocate_static_pie (DT_RELA
# kept intact), so the program runs from ext2 and writes its marker.  The old
# loader refusal row ("ELF load refused rc=-8") is now a failure.
hsz="$(stat -c %s "$X2/hs" 2>/dev/null)"
dbg "dump /lpx/out/static.txt $X2/static.txt" >/dev/null
l="$(head -1 "$X2/static.txt" 2>/dev/null)"
if grep -aq "ELF load refused rc=-8 size=$hsz file=/EXT/lpx/bin/hs" "$D/boot3.console.log"; then
  bad "static-pie /EXT/lpx/bin/hs: refused by the loader (IRELATIVE regression)"
fi
case "$l" in
  "ok static "*"libc=2.31 libcpath=static"*) good "/lpx/out/static.txt: $l (glibc static-pie, $hsz bytes, from ext2)" ;;
  *) bad "static-pie /EXT/lpx/bin/hs: /lpx/out/static.txt = '$l'" ;;
esac

if [ "$fails" = 0 ]; then
  say "ext2_gate: PASS"
else
  say "ext2_gate: FAIL ($fails)"
fi
if [ "${GATE_KEEP:-0}" != 1 ]; then rm -f "$IMG"; fi
[ "$fails" = 0 ]
