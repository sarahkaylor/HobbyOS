#!/bin/bash
# =============================================================================
# tools/nv/deploy.sh -- build a HobbyOS x86_64 image, boot it on Proxmox r1 in
# VM 300, optionally with the RTX 4090 passed through, and pull the serial log.
#
# This is the harness docs/nvidia-driver.md's evidence came from.  Measured
# loop on the dev box: build ~1 min (warm) + gzip transfer ~45 s for the 1 GiB
# image + boot to the driver rows ~25 s.
#
# Usage:
#   tools/nv/deploy.sh --mode unit_tests|test|desktop --gpu|--no-gpu [--wave NAME]
#
#   --gpu        attach 0000:01:00 (AD102) to VM 300 via hostpci0
#   --no-gpu     leave VM 300 without the card (control run)
#   --wave NAME  stage /WAVE.CFG naming one program (MODE=test only)
#
# Requires: ssh key ~/.ssh/mac_to_r1 for root@192.168.10.174, and a HobbyOS
# tree that can build for ARCH=intel ({mtools,mcopy} on the build host).
# =============================================================================
set -u

PVE_HOST="root@192.168.10.174"
SSH_KEY="${HOME}/.ssh/mac_to_r1"
VMID=300
REMOTE_DIR=/root/hobbyos-nv
MODE=test
GPU=0
WAVE=""
DISK_MB=${DISK_MB:-1024}

while [ $# -gt 0 ]; do
  case "$1" in
  --mode) MODE="$2"; shift 2 ;;
  --gpu) GPU=1; shift ;;
  --no-gpu) GPU=0; shift ;;
  --wave) WAVE="$2"; shift 2 ;;
  -h | --help) sed -n '2,25p' "$0"; exit 0 ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say() { echo "[deploy] $*"; }
r1() { ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no "$PVE_HOST" "$@"; }

if [ "${SKIP_BUILD:-0}" = "1" ]; then
  say "SKIP_BUILD=1: using the existing disk.img"
  [ -f disk.img ] || { echo "no disk.img to deploy" >&2; exit 1; }
else
  say "building ARCH=intel MODE=$MODE"
  # DESKTOP=hobby is required on x64: the icewm desktop's HBHOST.BIN is an
  # x86-only ts() path and this harness only needs a console that paints.
  # LO_ON_DISK= keeps the LibreOffice suite off the image: when
  # third_party/x11/.../libX11.a exists the Makefile defaults it to 1 and drags
  # the whole suite into every disk build (measured by the infra lane: minutes of
  # build for a file no driver probe opens).
  case "$MODE" in
  desktop) make ARCH=intel MODE=desktop DESKTOP=hobby LO_ON_DISK= -j"$(nproc 2>/dev/null || echo 8)" disk.img ;;
  *) make ARCH=intel MODE="$MODE" LO_ON_DISK= -j"$(nproc 2>/dev/null || echo 8)" disk.img ;;
  esac || { echo "build failed" >&2; exit 1; }
fi

# One program per boot: MODE=test runs the manifest and nothing else, so the
# console is attributable (docs/nvidia-driver.md section 5.1).
if [ -n "$WAVE" ]; then
  printf '%s\n' "$WAVE" > /tmp/nv-wave.cfg
  mcopy -o -i disk.img /tmp/nv-wave.cfg ::/WAVE.CFG || exit 1
  say "staged /WAVE.CFG = $WAVE"
fi

# VM 300 is created once with the shape the driver work needs: q35 + OVMF (so
# the image's EFI boot works), an NVMe disk because the kernel's virtio-blk path
# wants one, virtio-vga for the console/screenshot, serial to a file because a
# headless test boot has no other channel, and NO vIOMMU (docs/nvidia-driver.md
# section 8.4).  Re-running deploy.sh never recreates it.
if ! r1 "qm status $VMID" >/dev/null 2>&1; then
  say "creating VM $VMID"
  r1 "qm create $VMID --name hobbyos-nv --memory 16384 --cores 8 --sockets 1 \
      --cpu host --machine q35 --bios ovmf --vga virtio \
      --efidisk0 local-lvm:0,efitype=4m,pre-enrolled-keys=0" >/dev/null || exit 1
  # No net0: OVMF with a NIC can sit on "Start PXE over IPv4" instead of the
  # NVMe entry (measured by the infra lane).  The driver probe needs no network;
  # add one with `qm set 300 --net0 virtio,bridge=vmbr0` if a run ever does.
  # One quoted payload: single quotes inside a double-quoted remote command, so
  # nothing on this line needs another level of escaping.
  VM_ARGS="-drive file=$REMOTE_DIR/disk.img,format=raw,if=none,id=disk0 -device pcie-root-port,id=pcie.nvme,bus=pcie.0,slot=2 -device nvme,drive=disk0,serial=1234,bus=pcie.nvme -serial file:$REMOTE_DIR/serial.log -action shutdown=poweroff"
  r1 "qm set $VMID --args '$VM_ARGS'" >/dev/null || exit 1
fi

# Stage the GSP firmware when the fetch script has run.  The driver's next stage
# loads /NVIDIA/GSP/*.BIN through the normal file API, so a probe run has to
# carry them on the image -- 37 MB, which is why they live in a gitignored tree
# and are copied here rather than committed.  Set STAGE_FW=0 to keep them off.
if [ "${STAGE_FW:-1}" = "1" ] && [ -d third_party/nvfw/ga102/gsp ]; then
  mmd -i disk.img ::/NVIDIA 2>/dev/null || true
  mmd -i disk.img ::/NVIDIA/GSP 2>/dev/null || true
  # Role names, not vendor names: the image is FAT16 and HobbyOS's native path
  # resolver does not read long-filename entries, so /NVIDIA/GSP/booter_load-
  # 535.113.01.bin would only be reachable as BOOTER~1.BIN.  8.3 role names are
  # what the driver opens (see NV_FW_BOOTER in nv_gsp.c).
  stage_fw() { # <source> <8.3 name>
    [ -f "$1" ] || return 0
    mcopy -o -i disk.img "$1" "::/NVIDIA/GSP/$2" || exit 1
    say "staged $(basename "$1") as $2"
  }
  stage_fw "$(ls third_party/nvfw/ad102/gsp/booter_load-*.bin 2>/dev/null | head -1)" BOOTER.BIN
  stage_fw "$(ls third_party/nvfw/ad102/gsp/bootloader-*.bin 2>/dev/null | head -1)" LOADER.BIN
  stage_fw "$(ls third_party/nvfw/ga102/gsp/gsp-*.bin 2>/dev/null | head -1)" GSPRM.BIN
  stage_fw "$(ls third_party/nvfw/ad102/gsp/booter_unload-*.bin 2>/dev/null | head -1)" UNLOAD.BIN
fi

say "recreating VM $VMID on $PVE_HOST"
r1 "qm stop $VMID >/dev/null 2>&1; sleep 2; qm status $VMID" || true
if [ "$GPU" = 1 ]; then
  r1 "qm set $VMID --hostpci0 0000:01:00,pcie=1" >/dev/null || exit 1
  say "RTX 4090 (0000:01:00) attached"
else
  r1 "qm set $VMID --delete hostpci0" >/dev/null 2>&1 || true
  say "no GPU attached (control)"
fi

say "uploading disk.img (gzip in transit: 1 GiB -> ~45 s on the lab link)"
r1 "mkdir -p $REMOTE_DIR"
gzip -1 -c disk.img |
  r1 "gunzip -c > $REMOTE_DIR/disk.img.new && mv $REMOTE_DIR/disk.img.new $REMOTE_DIR/disk.img" || {
  echo "transfer failed" >&2
  exit 1
}

say "booting"
r1 "rm -f $REMOTE_DIR/serial.log; qm start $VMID >/dev/null"
# unit_tests/test self-terminate (the kernel halts QEMU); desktop runs forever.
if [ "$MODE" != "desktop" ]; then
  r1 "for i in \$(seq 1 90); do s=\$(qm status $VMID | awk '{print \$2}'); \
      [ \"\$s\" != running ] && break; sleep 5; done; qm status $VMID"
fi

OUT="evidence/nv/${MODE}$([ "$GPU" = 1 ] && echo "-gpu")-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$OUT")"
r1 "cat $REMOTE_DIR/serial.log" >"$OUT" 2>/dev/null
say "serial log: $OUT ($(wc -l <"$OUT" | tr -d ' ') lines)"
grep -E '^\[(NV|PCI|ACPI)\]|^nv:|^Tests (run|failed)' "$OUT" | tail -40

if [ "$MODE" = "desktop" ]; then
  say "VM $VMID left running (desktop).  Screenshot:"
  say "  printf '{\"execute\":\"qmp_capabilities\"}\n{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$REMOTE_DIR/scr.ppm\"}}\n' | ssh -i $SSH_KEY $PVE_HOST socat - unix-connect:/var/run/qemu-server/$VMID.qmp"
fi
