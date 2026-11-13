#!/bin/bash
# Read the probe's report back out of a Parallels plain disk, no serial port.
#
#   tools/probe/read_probe.sh [hdd-bundle]
#
# Tries, in order: \PROBE.TXT through mtools, then the raw sector dump the probe
# writes at LastLBA-96 (magic "HBPROBE1"), then prints how to get the screen.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HDD="${1:-$ROOT/obj/probe/probe.hdd}"
HDS="$(find "$HDD" -name '*.hds' -type f | head -1)"
[ -n "$HDS" ] || { echo "no .hds in $HDD" >&2; exit 1; }

ESP_OFF=$((1048576))   # mkgpt.py puts the ESP at LBA 2048

if mtype -i "$HDS@@$ESP_OFF" ::/PROBE.TXT 2>/dev/null; then
    exit 0
fi

# raw fallback: 96 sectors ending at the last LBA of a 128 MiB disk
TOTAL=$(( $(stat -f%z "$HDS") / 512 ))
LBA=$(( TOTAL - 1 - 96 ))
OUT="$ROOT/obj/probe/raw.bin"
dd if="$HDS" of="$OUT" bs=512 skip=$LBA count=96 status=none
if head -c 8 "$OUT" | grep -q HBPROBE1; then
    tail -c +9 "$OUT" | tr -d '\000'
    exit 0
fi

echo "(no PROBE.TXT and no raw dump: the probe did not reach the file stage)"
echo "the VM window holds the transcript; capture it with:"
echo "  screencapture -x /tmp/prl.png"
exit 1
