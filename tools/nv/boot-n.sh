#!/bin/bash
# =============================================================================
# tools/nv/boot-n.sh -- boot the image in this tree N times on VM 300 and keep
# one named serial log per boot.
#
# Why it exists: tools/nv/deploy.sh rebuilds, re-uploads and then waits up to
# 450 s for the guest to self-terminate.  The NV probe boots do not halt QEMU
# (MODE=test runs the manifest and the kernel sits at an idle prompt), so a
# four-boot measurement costs four rebuilds and four 450 s waits.  This does the
# part after the build -- firmware staging, one upload, one boot, one poll for
# the [NV-GSP] stage5 row, one serial pull, one stop -- per increment of N, and
# prints the image's sha256 and the boot's ptimer origin so four logs cannot be
# one boot reported twice.
#
# Usage: tools/nv/boot-n.sh <count> <tag>      (tag becomes r6a, r6b, ...)
# Requires the same things deploy.sh does: ssh key ~/.ssh/mac_to_r1, the GSP
# firmware staged in third_party/nvfw, and a disk.img already built.
# =============================================================================
set -u

COUNT=${1:-1}
TAG=${2:-boot}
PVE_HOST="root@192.168.10.174"
SSH_KEY="${HOME}/.ssh/mac_to_r1"
VMID=300
REMOTE_DIR=/root/hobbyos-nv
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 1

r1() { ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no "$PVE_HOST" "$@"; }
say() { echo "[boot-n] $*"; }

[ -f disk.img ] || { echo "no disk.img: build it first" >&2; exit 1; }

# --- firmware, exactly the roles deploy.sh stages (same 8.3 names) -----------
if [ -d third_party/nvfw/ga102/gsp ]; then
  mmd -i disk.img ::/NVIDIA 2>/dev/null || true
  mmd -i disk.img ::/NVIDIA/GSP 2>/dev/null || true
  stage() { [ -f "$1" ] || return 0; mcopy -o -i disk.img "$1" "::/NVIDIA/GSP/$2" || exit 1; }
  stage "$(ls third_party/nvfw/ad102/gsp/booter_load-*.bin 2>/dev/null | head -1)" BOOTER.BIN
  stage "$(ls third_party/nvfw/ad102/gsp/bootloader-*.bin 2>/dev/null | head -1)" LOADER.BIN
  stage "$(ls third_party/nvfw/ga102/gsp/gsp-*.bin 2>/dev/null | head -1)" GSPRM.BIN
  stage "$(ls third_party/nvfw/ad102/gsp/booter_unload-*.bin 2>/dev/null | head -1)" UNLOAD.BIN
  say "firmware staged: $(mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -c BIN) file(s)"
else
  say "WARNING: third_party/nvfw/ga102/gsp missing -- the image will boot without GSP firmware"
fi

# Prove the firmware is really in the image: without it the driver's stage 5
# never runs and the boot produces no [NV-SEC2] rows at all (that is exactly how
# a torn upload of a concurrently rebuilt disk.img shows up).
if mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -q BOOTER; then
  say "BOOTER.BIN present in the image"
else
  say "FATAL: BOOTER.BIN is not in disk.img -- refusing to spend a boot"
  exit 1
fi

IMG_SHA=$(sha256sum disk.img | cut -c1-64)
say "local image sha256=$IMG_SHA size=$(stat -c %s disk.img)"
mkdir -p evidence/nv

letters=(a b c d e f g h)
for i in $(seq 1 "$COUNT"); do
  name="${TAG}${letters[$((i - 1))]}"
  say "--- $name ($i/$COUNT) ---"
  r1 "qm stop $VMID >/dev/null 2>&1; sleep 2"
  say "uploading to $REMOTE_DIR/disk.img"
  gzip -1 -c disk.img | r1 "gunzip -c > $REMOTE_DIR/disk.img.new && mv $REMOTE_DIR/disk.img.new $REMOTE_DIR/disk.img" || {
    say "upload failed"; exit 1; }
  r1 "rm -f $REMOTE_DIR/serial.log; qm start $VMID >/dev/null" || { say "qm start failed"; exit 1; }
  got=0
  for _ in $(seq 120); do
    sleep 2
    if r1 "grep -q 'stage5 result' $REMOTE_DIR/serial.log 2>/dev/null"; then got=1; break; fi
  done
  sleep 3
  r1 "cat $REMOTE_DIR/serial.log" > "evidence/nv/$name.log"
  rows=$(grep -c 'halt-timing' "evidence/nv/$name.log" 2>/dev/null || true)
  t0=$(grep -o 'ptimer t0=0x[0-9A-F]*' "evidence/nv/$name.log" | head -1)
  say "$name stage5_seen=$got lines=$(wc -l < "evidence/nv/$name.log") halt-timing_rows=$rows $t0"
  r1 "qm stop $VMID >/dev/null 2>&1; sleep 2"
  [ "$got" = 1 ] && [ "${rows:-0}" -ge 1 ] || say "WARNING: $name has no halt-timing row -- do not use it as evidence"
done

say "VM $VMID: $(r1 "qm status $VMID" 2>&1)"
say "VM 312: $(r1 "qm status 312" 2>&1)"
say "qemu -id $VMID processes: $(r1 "ps -eo args | grep -- '-id $VMID' | grep -c qemu" 2>/dev/null || echo 0)"
