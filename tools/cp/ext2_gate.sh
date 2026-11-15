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
#
# Every QEMU boot runs under `flock /tmp/cp-qemu.lock` (shared CI box).
# Env: GATE_DIR (default /tmp/cp-ext2-gate), CPDATA_SIZE (default 8G),
#      GATE_KEEP=1 keeps the image.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
D="${GATE_DIR:-/tmp/cp-ext2-gate}"
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
  got="$(dbg "ls -p /lp" | awk -F/ 'NF>5 && $6!="." && $6!=".." {print $6}' | sort | tr '\n' ' ')"
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

boot 1
check_image 1
boot 2
grep -aq '^\[LP\] phase2 complete' "$D/boot2.console.log" && good "boot 2 ran phase 2 (persistence)" || bad "boot 2 did not reach phase 2"
check_image 2

if [ "$fails" = 0 ]; then
  say "ext2_gate: PASS"
else
  say "ext2_gate: FAIL ($fails)"
fi
if [ "${GATE_KEEP:-0}" != 1 ]; then rm -f "$IMG"; fi
[ "$fails" = 0 ]
