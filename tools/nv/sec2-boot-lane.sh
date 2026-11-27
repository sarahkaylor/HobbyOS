#!/bin/bash
# =============================================================================
# tools/nv/sec2-boot-lane.sh -- stage a named booter, assert it, then boot.
#
# This is the lane's only documented path to a SEC2 boot on r1 (VM 300).  It
# stages the firmware into this tree's disk.img, proves on the console which
# booter container is in that image, and then boots it N times, keeping one
# serial log per boot.
#
# Usage: tools/nv/sec2-boot-lane.sh <count> <tag>      (tag becomes t10a, ...)
#
#   NV_BOOTER_FILE=<path>            exact booter container to stage
#                                    (default: first
#                                    third_party/nvfw/ad102/gsp/booter_load-*.bin
#                                    -- the 535 control, which is NOT
#                                    necessarily the candidate under test)
#   NV_EXPECT_BOOTER_SHA256=<hex>    pin that container; a mismatch refuses
#                                    before the image is uploaded or a VM is
#                                    touched.  NV_BOOTER_SHA256 is the old
#                                    spelling and still works.
#   count=0                          dry run: preflight (and staging, when
#                                    disk.img exists), no r1 call at all.
#
# What the boot proves before it is allowed to happen -- tools/nv/
# booter_preflight.py prints these rows and refuses on any of them:
#
#   booter staged: <sha256> size=<n> path=<p>     the file that will be staged
#   booter pin: <hex> | none                      the pin in force
#   booter header: ...                            parsed bin_hdr/HS/load header
#   booter geometry: ...                          the bound derived from the
#                                                 container's OWN load header
#   booter REFUSED: <what> -- <why>               exit 2 absent, 3 geometry,
#                                                 4 sha mismatch, 5 bad pin
#
# The geometry bound is not the 0x7700 + 0x6000 constant: it is what the load
# header accounts for (data_off + data_size, and the IMEM/DMEM extents the
# guest derives from it).  A bootloader-only container (data_size == 0) is
# bounded by its own os_code extent, which is the case docs/nv-no-rm-path.md §6
# names.  After staging, the image is read back out of disk.img and hashed
# again, so "staged" means the bytes in the image, not the bytes in the
# directory.
#
# Exit codes: 0 kept/ok, 1 staging or boot failure, 2 booter absent,
#             3 booter geometry, 4 booter sha mismatch, 5 bad pin/usage.
# =============================================================================
set -u

COUNT=${1:-1}
TAG=${2:-s1}
VMID=300
REMOTE_DIR=/root/hobbyos-nv
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 1

r1() { ssh -F "$HOME/.ssh/config" -o BatchMode=yes -o StrictHostKeyChecking=no r1 "$@"; }
say() { echo "[sec2-lane] $*"; }

case "$COUNT" in
''|*[!0-9]*) say "FATAL: count '$COUNT' is not a number"; exit 5 ;;
esac
[ "$COUNT" -le 8 ] || { say "FATAL: count $COUNT > 8 (tags are lettered a..h)"; exit 5; }

PREFLIGHT="$HERE/tools/nv/booter_preflight.py"
[ -f "$PREFLIGHT" ] || { say "FATAL: $PREFLIGHT is missing from this tree"; exit 1; }

# ---- the pin, under whichever name the caller used -------------------------
EXPECT=${NV_EXPECT_BOOTER_SHA256:-}
if [ -z "$EXPECT" ] && [ -n "${NV_BOOTER_SHA256:-}" ]; then
  EXPECT=$NV_BOOTER_SHA256
  say "note: NV_BOOTER_SHA256 is the old spelling; use NV_EXPECT_BOOTER_SHA256"
elif [ -n "$EXPECT" ] && [ -n "${NV_BOOTER_SHA256:-}" ] && [ "$EXPECT" != "$NV_BOOTER_SHA256" ]; then
  say "FATAL: NV_EXPECT_BOOTER_SHA256=$EXPECT != NV_BOOTER_SHA256=$NV_BOOTER_SHA256 -- two pins disagree"
  exit 5
fi

# ---- 1. which booter, and is it the one we meant? -------------------------
# The boot is the expensive unit.  Everything below runs before the first r1
# call, so a refusal costs no VM and no upload.
BOOTER=${NV_BOOTER_FILE:-}
if [ -z "$BOOTER" ]; then
  BOOTER=$(ls third_party/nvfw/ad102/gsp/booter_load-*.bin 2>/dev/null | head -1)
  [ -n "$BOOTER" ] && say "booter default: $BOOTER (no NV_BOOTER_FILE; this is the first globbed booter_load, often the 535 control)"
fi
[ -n "$BOOTER" ] || {
  say "FATAL: no booter to stage: NV_BOOTER_FILE is unset and no third_party/nvfw/ad102/gsp/booter_load-*.bin exists"
  exit 2; }

rc=0
python3 "$PREFLIGHT" --expect-sha "$EXPECT" "$BOOTER" || rc=$?
case "$rc" in
0) : ;;
2) say "FATAL: the booter named by NV_BOOTER_FILE is not there; a typo must not boot the previous image"; exit 2 ;;
3) say "FATAL: the container's own load header does not account for this file; refusing to stage it"; exit 3 ;;
4) say "FATAL: the staged image is not the pinned candidate; refusing to spend a boot on it"; exit 4 ;;
5) say "FATAL: the pin is not a 64-digit hex sha256"; exit 5 ;;
*) say "FATAL: booter_preflight.py exited $rc"; exit "$rc" ;;
esac

if [ -z "$EXPECT" ] && [ "$COUNT" -gt 0 ]; then
  say "WARNING: no NV_EXPECT_BOOTER_SHA256: this boot asserts what it staged, but nothing pinned it to a candidate"
fi

# ---- 2. stage, then prove the image holds those bytes ---------------------
if [ ! -f disk.img ]; then
  if [ "$COUNT" -gt 0 ]; then
    say "FATAL: no disk.img: build it first (make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 -j8 disk.img)"
    exit 1
  fi
  say "booter stage: skipped (no disk.img; preflight only)"
  say "vm check: skipped (count=0 dry run; r1 was not contacted)"
  exit 0
fi

mmd -i disk.img ::/NVIDIA 2>/dev/null || true
mmd -i disk.img ::/NVIDIA/GSP 2>/dev/null || true
stage() { # <path> <8.3 name> <what was looked for>
  [ -f "$1" ] || { say "WARNING: $2 not staged: no file at '${1:-${3:-<none>}}'"; return 0; }
  mcopy -o -i disk.img "$1" "::/NVIDIA/GSP/$2" || { say "FATAL: mcopy $1 -> $2 failed"; exit 1; }
}
stage "$BOOTER" BOOTER.BIN "NV_BOOTER_FILE"
stage "$(ls third_party/nvfw/ad102/gsp/bootloader-*.bin 2>/dev/null | head -1)" LOADER.BIN \
      "third_party/nvfw/ad102/gsp/bootloader-*.bin"
stage "$(ls third_party/nvfw/ga102/gsp/gsp-*.bin 2>/dev/null | head -1)" GSPRM.BIN \
      "third_party/nvfw/ga102/gsp/gsp-*.bin"
stage "$(ls third_party/nvfw/ad102/gsp/booter_unload-*.bin 2>/dev/null | head -1)" UNLOAD.BIN \
      "third_party/nvfw/ad102/gsp/booter_unload-*.bin"
say "firmware staged: $(mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -c BIN) file(s)"

mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -q BOOTER || {
  say "FATAL: BOOTER.BIN is not in disk.img"; exit 1; }

# The row above says what we meant to stage.  This one says what is in the
# image: read it back out and hash it.  A mismatch here means the staging step
# lied (long-name collision, stale FAT entry) and no boot may follow.
STAGED=$(mktemp "${TMPDIR:-/tmp}/booter-staged.XXXXXX") || exit 1
trap 'rm -f "$STAGED"' EXIT
mcopy -n -i disk.img ::/NVIDIA/GSP/BOOTER.BIN "$STAGED" || {
  say "FATAL: could not read BOOTER.BIN back out of disk.img"; exit 1; }
WANT_SHA=$(sha256sum "$BOOTER" | cut -c1-64)
GOT_SHA=$(sha256sum "$STAGED" | cut -c1-64)
GOT_SIZE=$(stat -c %s "$STAGED")
if [ "$GOT_SHA" != "$WANT_SHA" ]; then
  say "booter REFUSED: image holds sha256=$GOT_SHA size=$GOT_SIZE but '$BOOTER' is sha256=$WANT_SHA -- refusing to boot a different container"
  exit 4
fi
say "booter staged-verified: sha256=$GOT_SHA size=$GOT_SIZE image=::/NVIDIA/GSP/BOOTER.BIN"

IMG_SHA=$(sha256sum disk.img | cut -c1-64)
say "local image sha256=$IMG_SHA size=$(stat -c %s disk.img)"

if [ "$COUNT" -eq 0 ]; then
  say "vm check: skipped (count=0 dry run; r1 was not contacted)"
  exit 0
fi

mkdir -p evidence/nv

# Nobody else may be using the card.  VM 300 owns 0000:01:00 while it runs.
case "$(r1 "qm status $VMID" 2>&1)" in
*stopped*) : ;;
*) say "FATAL: VM $VMID is not stopped -- refusing to collide with another lane"; exit 1 ;;
esac

letters=(a b c d e f g h)
for i in $(seq 1 "$COUNT"); do
  name="${TAG}${letters[$((i - 1))]}"
  say "--- $name ($i/$COUNT) booter=$BOOTER sha256=$WANT_SHA ---"
  r1 "qm stop $VMID >/dev/null 2>&1; sleep 2"
  gzip -1 -c disk.img | r1 "gunzip -c > $REMOTE_DIR/disk.img.new && mv $REMOTE_DIR/disk.img.new $REMOTE_DIR/disk.img" || {
    say "upload failed"; exit 1; }
  r1 "rm -f $REMOTE_DIR/serial.log; qm start $VMID >/dev/null" || { say "qm start failed"; exit 1; }
  got=0
  for _ in $(seq 150); do
    sleep 2
    if r1 "grep -q 'stage5 result' $REMOTE_DIR/serial.log 2>/dev/null"; then got=1; break; fi
  done
  sleep 4
  r1 "cat $REMOTE_DIR/serial.log" > "evidence/nv/$name.log"
  rows=$(grep -c 'NV-SEC2T' "evidence/nv/$name.log" 2>/dev/null || true)
  sweep=$(grep -c 'ctl-write-sweep' "evidence/nv/$name.log" 2>/dev/null || true)
  t0=$(grep -o 'ptimer t0=0x[0-9A-F]*' "evidence/nv/$name.log" | head -1)
  say "$name stage5_seen=$got lines=$(wc -l < "evidence/nv/$name.log") sec2t_rows=$rows sweep_rows=$sweep $t0"
  r1 "qm stop $VMID >/dev/null 2>&1; sleep 2"
  if [ "$got" = 1 ] && [ "${rows:-0}" -ge 6 ] && [ "${sweep:-0}" -ge 1 ]; then
    say "$name KEPT (trace rows present)"
  else
    say "WARNING: $name is missing trace rows -- do not use it as evidence"
  fi
done

say "VM $VMID: $(r1 "qm status $VMID" 2>&1)"
say "VM 312: $(r1 "qm status 312" 2>&1)"
say "qemu processes on r1: $(r1 'ps -eo comm | grep -c qemu-system' 2>/dev/null || echo 0)"
