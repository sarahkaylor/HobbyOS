#!/bin/bash
# =============================================================================
# tools/nv/sec2-boot.sh -- boot this tree's disk.img N times on VM 300 (r1) and
# keep one named serial log per boot.
#
# Round 7's question is a register trace, so the thing that makes a log usable
# is that the trace rows are present and that the image is the same bytes in
# every boot.  This script refuses to spend a boot on an image without the
# SEC2 trace, prints the image's sha256 and the boot's ptimer origin so N logs
# cannot be one boot counted N times, and checks the diagnostic greps before it
# keeps a log.
#
# Usage: tools/nv/sec2-boot.sh <count> <tag>       (tag becomes t7a, t7b, ...)
# =============================================================================
set -u

COUNT=${1:-1}
TAG=${2:-t7}
PVE_HOST="root@192.168.10.174"
SSH_KEY="${HOME}/.ssh/mac_to_r1"
VMID=300
REMOTE_DIR=/root/hobbyos-nv
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 1

r1() { ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no "$PVE_HOST" "$@"; }
say() { echo "[sec2-boot] $*"; }

[ -f disk.img ] || { echo "no disk.img: build it first" >&2; exit 1; }

# Firmware, exactly the roles deploy.sh stages (same 8.3 names).
mmd -i disk.img ::/NVIDIA 2>/dev/null || true
mmd -i disk.img ::/NVIDIA/GSP 2>/dev/null || true
stage() { [ -f "$1" ] || return 0; mcopy -o -i disk.img "$1" "::/NVIDIA/GSP/$2" || exit 1; }
stage "$(ls third_party/nvfw/ad102/gsp/booter_load-*.bin 2>/dev/null | head -1)" BOOTER.BIN
stage "$(ls third_party/nvfw/ad102/gsp/bootloader-*.bin 2>/dev/null | head -1)" LOADER.BIN
stage "$(ls third_party/nvfw/ga102/gsp/gsp-*.bin 2>/dev/null | head -1)" GSPRM.BIN
stage "$(ls third_party/nvfw/ad102/gsp/booter_unload-*.bin 2>/dev/null | head -1)" UNLOAD.BIN
say "firmware staged: $(mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -c BIN) file(s)"

# A torn image or a missing booter produces no [NV-SEC2] rows at all; refuse to
# spend a boot on one.
mdir -i disk.img ::/NVIDIA/GSP/ 2>/dev/null | grep -q BOOTER || {
  say "FATAL: BOOTER.BIN is not in disk.img"; exit 1; }

IMG_SHA=$(sha256sum disk.img | cut -c1-64)
say "local image sha256=$IMG_SHA size=$(stat -c %s disk.img)"
mkdir -p evidence/nv

# Nobody else may be using the card.  VM 300 owns 0000:01:00 while it runs.
case "$(r1 "qm status $VMID" 2>&1)" in
*stopped*) : ;;
*) say "FATAL: VM $VMID is not stopped -- refusing to collide with another lane"; exit 1 ;;
esac

letters=(a b c d e f g h)
for i in $(seq 1 "$COUNT"); do
  name="${TAG}${letters[$((i - 1))]}"
  say "--- $name ($i/$COUNT) ---"
  r1 "qm stop $VMID >/dev/null 2>&1; sleep 2"
  gzip -1 -c disk.img | r1 "gunzip -c > $REMOTE_DIR/disk.img.new && mv $REMOTE_DIR/disk.img.new $REMOTE_DIR/disk.img" || {
    say "upload failed"; exit 1; }
  r1 "rm -f $REMOTE_DIR/serial.log; qm start $VMID >/dev/null" || { say "qm start failed"; exit 1; }
  got=0
  for _ in $(seq 150); do
    sleep 2
    if r1 "grep -q 'stage5 result' $REMOTE_DIR/serial.log 2>/dev/null"; then got=1; break; fi
  done
  sleep 3
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
