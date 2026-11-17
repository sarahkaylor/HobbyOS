#!/usr/bin/env bash
# tools/cp/venus/run_probe.sh -- boot ONE Linux-ABI probe on the Venus host
# recipe (Gate P6, lane C6-venus).  Same protocol as tools/gate/t3_linux_probe.sh
# ("[LP] ..." rows, "[LP] done rc=N"), but the guest gets the Venus device
# (tools/cp/host-venus.sh) instead of the plain virtio-gpu the t3 tier uses.
#
#   bash tools/cp/venus/run_probe.sh SRC.c [ARGS...]
#
# SRC.c is built musl static-PIE with -Isrc/include (the /dev/gpu ABI header).
# Env: VP_WORK (obj/venus-probe), VP_TIMEOUT (240 s), VP_NO_BUILD=1 (reuse the
# MODE=test kernel/disk already in the tree), plus host-venus.sh's env.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
SRC=${1:?usage: run_probe.sh SRC.c [ARGS...]}; shift
NAME=$(basename "$SRC" .c)
WORK=${VP_WORK:-$REPO/obj/venus-probe}
TIMEOUT=${VP_TIMEOUT:-240}
mkdir -p "$WORK"
cd "$REPO"
if [ "${VP_NO_BUILD:-0}" != 1 ]; then
    rm -f hobbyos.elf
    TOOLS_SKIP=1 make ARCH=intel MODE=test hobbyos.elf disk.img > "$WORK/build.log" 2>&1 || {
        tail -30 "$WORK/build.log"; echo "run_probe: kernel/disk build FAILED"; exit 2; }
fi
musl-gcc -static-pie -fPIE -O2 -g -Wall -Wextra -Isrc/include -o "$WORK/$NAME.elf" "$SRC"
readelf -h "$WORK/$NAME.elf" | grep -q 'Type:.*DYN' || { echo "run_probe: not static-PIE"; exit 2; }
cp --sparse=always disk.img "$WORK/disk.img"
printf 'PROBE.ELF %s\n' "$*" > "$WORK/WAVE.CFG"
mcopy -o -i "$WORK/disk.img" "$WORK/$NAME.elf" ::PROBE.ELF
mcopy -o -i "$WORK/disk.img" "$WORK/WAVE.CFG" ::WAVE.CFG
rm -f "$WORK/serial.log"
bash tools/cp/qemu-slot.sh bash tools/cp/host-venus.sh boot "$WORK/disk.img" "$WORK" &
outer=$!
t0=$(date +%s); qpid=
while :; do
    sleep 2
    [ -z "$qpid" ] && qpid=$(pgrep -f "^qemu-system-x86_64 -name hobbyos-venus.*$WORK/serial.log" | head -1 || true)
    grep -aq '\[LP\] done rc=' "$WORK/serial.log" 2>/dev/null && { sleep 2; break; }
    [ $(( $(date +%s) - t0 )) -ge "$TIMEOUT" ] && { echo "run_probe: TIMEOUT after ${TIMEOUT}s"; break; }
    kill -0 "$outer" 2>/dev/null || break
done
[ -n "$qpid" ] && kill "$qpid" 2>/dev/null || true
wait "$outer" 2>/dev/null || true
grep -aE '^\[VIRGL3D\] (feat|shm|irq|capset index)|\[VENUS\]|\[LP\]|\[LCOMPAT\]|FATAL|fault' "$WORK/serial.log" > "$WORK/rows.txt" || true
cat "$WORK/rows.txt"
echo "--- qemu stderr (tail)"; tail -15 "$WORK/qemu-stderr.txt" 2>/dev/null || true
grep -aq '\[LP\] done rc=0' "$WORK/serial.log"
