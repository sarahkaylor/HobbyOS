#!/usr/bin/env bash
# cp/p0guest: fetch the official Ubuntu 26.04 cloud image and verify it.
#   1. SHA256SUMS is checked against SHA256SUMS.gpg with the distro's
#      ubuntu-cloudimage-keyring (ships in ubuntu-keyring / is on strix).
#   2. The image is checked against SHA256SUMS.
# Output: ~/cp/p0guest/img/ubuntu-26.04-server-cloudimg-amd64.img (read-only base).
set -euo pipefail
ROOT=${CP_P0GUEST:-$HOME/cp/p0guest}
REL=${REL:-26.04}
BASE=https://cloud-images.ubuntu.com/releases/$REL/release
IMG=ubuntu-$REL-server-cloudimg-amd64.img
KEYRING=/usr/share/keyrings/ubuntu-cloudimage-keyring.gpg
mkdir -p "$ROOT/img"
cd "$ROOT/img"
curl -fsSL -o SHA256SUMS     "$BASE/SHA256SUMS"
curl -fsSL -o SHA256SUMS.gpg "$BASE/SHA256SUMS.gpg"
gpgv --keyring "$KEYRING" SHA256SUMS.gpg SHA256SUMS
if [ ! -f "$IMG" ] || ! grep " \*$IMG\$" SHA256SUMS | sha256sum -c --quiet - ; then
    curl -fL -o "$IMG.part" "$BASE/$IMG"
    mv "$IMG.part" "$IMG"
fi
grep " \*$IMG\$" SHA256SUMS | sha256sum -c -
chmod a-w "$IMG"
ls -l "$IMG"
