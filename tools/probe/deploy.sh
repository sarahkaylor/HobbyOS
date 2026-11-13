#!/bin/bash
# Build the probe, wrap it in a Parallels plain .hdd, and point a VM at it.
#
#   tools/probe/deploy.sh <vm-name>      build + attach as hdd0
#   tools/probe/deploy.sh                build only, print the image path
#
# Why the two-step dance: Parallels refuses a raw image ("The specified hard
# disk image file is invalid") and prl_disk_tool cannot import raw, but a
# *plain* Parallels disk is exactly the raw bytes, so we create an empty plain
# disk and dd the GPT image into its .hds data file.  The guest then boots it
# and can write to it, which is how the probe reports back without a serial port.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM="${1:-}"
DT="/Applications/Parallels Desktop.app/Contents/MacOS/prl_disk_tool"
OUT="$ROOT/obj/probe"
HDD="$OUT/probe.hdd"

bash "$ROOT/tools/probe/build.sh" >/dev/null

rm -rf "$HDD"
"$DT" create --hdd "$HDD" --size 128M >/dev/null
HDS="$(find "$HDD" -name '*.hds' -type f | head -1)"
[ -n "$HDS" ] || { echo "no .hds data file in $HDD" >&2; exit 1; }
dd if="$OUT/probe.img" of="$HDS" bs=1m status=none

echo "probe disk: $HDD"
if [ -n "$VM" ]; then
    prlctl stop "$VM" --kill >/dev/null 2>&1 || true
    prlctl set "$VM" --device-set hdd0 --image "$HDD" >/dev/null
    prlctl start "$VM" >/dev/null
    echo "booting $VM with the probe"
fi
