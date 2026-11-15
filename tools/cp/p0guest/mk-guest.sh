#!/usr/bin/env bash
# cp/p0guest: build the guest disk (qcow2 overlay on the verified cloud image)
# and the cloud-init NoCloud seed ISO. Idempotent; pass --reset to recreate
# the overlay (wipes the guest).
#
# Guest user: cp (passwordless sudo, ssh key only, no password set).
# Needs on the host: qemu-img (qemu-utils), genisoimage, ssh-keygen.
set -euo pipefail
ROOT=${CP_P0GUEST:-$HOME/cp/p0guest}
REL=${REL:-26.04}
BASEIMG=$ROOT/img/ubuntu-$REL-server-cloudimg-amd64.img
DISK=$ROOT/img/guest.qcow2
SEED=$ROOT/img/seed.iso
SIZE=${SIZE:-96G}
mkdir -p "$ROOT/ssh" "$ROOT/img" "$ROOT/seed"
[ -f "$ROOT/ssh/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C cp-p0guest -f "$ROOT/ssh/id_ed25519"
PUB=$(cat "$ROOT/ssh/id_ed25519.pub")

if [ "${1:-}" = --reset ] || [ ! -f "$DISK" ]; then
    rm -f "$DISK"
    qemu-img create -q -f qcow2 -F qcow2 -b "$BASEIMG" "$DISK" "$SIZE"
fi

cat > "$ROOT/seed/meta-data" <<EOF
instance-id: cp-p0guest-1
local-hostname: cp-p0guest
EOF
cat > "$ROOT/seed/user-data" <<EOF
#cloud-config
users:
  - name: cp
    groups: [sudo, video, render, input]
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    lock_passwd: true
    ssh_authorized_keys:
      - $PUB
ssh_pwauth: false
package_update: false
growpart: {mode: auto, devices: ['/']}
mounts:
  - [cp2077, /mnt/cp2077, virtiofs, "ro,nofail", "0", "0"]
  - [proton, /mnt/proton, virtiofs, "ro,nofail", "0", "0"]
runcmd:
  - [mkdir, -p, /mnt/cp2077, /mnt/proton]
  - [mount, -a]
EOF
genisoimage -quiet -output "$SEED" -volid cidata -joliet -rock "$ROOT/seed/user-data" "$ROOT/seed/meta-data"
ls -l "$DISK" "$SEED"
