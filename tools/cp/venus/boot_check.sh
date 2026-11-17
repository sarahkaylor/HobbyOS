#!/usr/bin/env bash
# tools/cp/venus/boot_check.sh -- Gate P6 step 1: boot the tree's disk.img on the
# Venus host recipe (tools/cp/host-venus.sh) and show what the kernel sees:
# the offered features, the host-visible shared-memory window and every capset
# (Venus = id 4).  One QEMU slot, held for the boot only.
#
#   bash tools/cp/venus/boot_check.sh [WORK] [SECS]
#
# WORK defaults to obj/venus-check; SECS (default 90) bounds the boot.  The
# rows are printed and written to WORK/rows.txt.  Exit 0 when a Venus capset
# row was seen.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
WORK=${1:-$REPO/obj/venus-check}
SECS=${2:-90}
shift $(( $# < 2 ? $# : 2 ))   # the rest are extra QEMU args
NEEDLE=${NEEDLE:-"(venus)"}
mkdir -p "$WORK"
cp --sparse=always "$REPO/disk.img" "$WORK/disk.img"
rm -f "$WORK/serial.log"
(
    cd "$REPO"
    bash tools/cp/qemu-slot.sh bash tools/cp/host-venus.sh boot "$WORK/disk.img" "$WORK" "$@" &
    wait
) &
outer=$!
t0=$(date +%s)
qpid=
while :; do
    sleep 2
    [ -z "$qpid" ] && qpid=$(pgrep -f "^qemu-system-x86_64 -name hobbyos-venus.*$WORK/serial.log" | head -1 || true)
    if grep -aq -- "$NEEDLE" "$WORK/serial.log" 2>/dev/null && grep -aq "VirtIO GPU" "$WORK/serial.log"; then
        sleep "${SETTLE:-5}"; break
    fi
    [ $(( $(date +%s) - t0 )) -ge "$SECS" ] && break
    kill -0 "$outer" 2>/dev/null || break
done
[ -n "$qpid" ] && kill "$qpid" 2>/dev/null || true
wait "$outer" 2>/dev/null || true
grep -aE '^\[VIRGL3D\]|VirtIO GPU|\[VENUS\]' "$WORK/serial.log" | tee "$WORK/rows.txt" || true
echo "--- qemu stderr (tail)"; tail -20 "$WORK/qemu-stderr.txt" 2>/dev/null || true
grep -aq -- "$NEEDLE" "$WORK/serial.log"
