#!/usr/bin/env bash
# tools/cp/fault/fp.sh KIND RUN -- H7-fault: one run-hunt.sh game boot of THIS
# tree (fp720 / soak / p8v ...) with the host-side NFS sampler (hostnet.sh)
# beside it, then the fault/NFS summary (pfsum.py).  Logs and summaries go to
# $FAULT_LOGS (default ~/cp/lanes/fault-logs); the data disk is dropped by
# run-hunt.sh, the shots here.  Audio: FAULT_AUDIO=1 (QEMU wav backend only,
# the WAV is checked and deleted by run-hunt.sh).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
KIND="${1:?kind}"; RUN="${2:?run}"
L="${FAULT_LOGS:-$HOME/cp/lanes/fault-logs}"; mkdir -p "$L"
export PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1
export CP_WINE_STAGE=$HOME/cp/wine-cp2/stage/CP/WINE WINE_BUILD=$HOME/cp/wine-cp2
export HUNT_LOGS="$L" HUNT_EXPORT="${FAULT_EXPORT:-/srv/nfs/cp-fault-$(basename "$ROOT")}"
[ "${FAULT_AUDIO:-0}" = 1 ] && export HUNT_AUDIO=1
case "$KIND" in p8*|win) D=cp-p8 ;; *) D=cp-p9 ;; esac
bash tools/cp/fault/hostnet.sh "obj/$D/$RUN/" "$L/$RUN.hostnet.txt" &
HP=$!
bash tools/cp/hunt/run-hunt.sh "$KIND" "$RUN" > "$L/$RUN.fp.out" 2>&1
rc=$?
wait "$HP" 2>/dev/null
echo "rc=$rc $(date +%T)" >> "$L/$RUN.fp.out"
[ -f tools/cp/audio2/gamecheck.sh ] && [ "${FAULT_AUDIO:-0}" = 1 ] && bash tools/cp/audio2/gamecheck.sh "$L" "$RUN" > "$L/$RUN.gamecheck" 2>&1
python3 -I tools/cp/fault/pfsum.py "$L/$RUN.serial.txt" --phases "$L/$RUN.phases.txt" \
  --hostnet "$L/$RUN.hostnet.txt" > "$L/$RUN.pfsum.txt" 2>&1
rm -rf "obj/$D/$RUN/shots" obj/$D/$RUN/*.png obj/$D/$RUN/*.ppm "obj/$D/$RUN/cpdata.img" "obj/$D/$RUN/disk.img" 2>/dev/null
exit $rc
