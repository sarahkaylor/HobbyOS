#!/usr/bin/env bash
# cp/p0guest (host): every measured run holds ~/cp/measure.lock (shared with
# lane C7-p0host) and writes raw output to ~/cp/p0guest/results/.
#   measure.sh guest vkcube|d3d12|fence   rows from inside the guest (VM must be up)
#   measure.sh host  d3d12|fence          the same probe on host RADV, no VM in the way
set -euo pipefail
ROOT=${CP_P0GUEST:-$HOME/cp/p0guest}
SECS=${SECS:-15}
mkdir -p "$ROOT/results"
where=$1 what=$2
out=$ROOT/results/$(date +%Y%m%d-%H%M%S)-$where-$what.txt
{
    echo "# $where $what $(date -Is) host=$(hostname) cpu=$(cat "$ROOT/run/cpu.txt" 2>/dev/null || echo -)"
    echo "# waiting for ~/cp/measure.lock"
} | tee "$out"
exec 9>"$HOME/cp/measure.lock"
flock 9
echo "# lock acquired $(date -Is)" | tee -a "$out"
case "$where" in
guest)
    bash "$ROOT/tools/host-venus.sh" ssh "SECS=$SECS bash tools/guest-ladder.sh $what" 2>&1 | tee -a "$out" ;;
host)
    p() { echo "### host-radv $* d3d12probe"; bash "$ROOT/tools/host-d3d12probe.sh" "$@" 2>/dev/null | grep -E '^(START|RESULT|FAIL|ADAPTER)'; }
    case "$what" in
    d3d12)
        for geom in 1280x720 1920x1080; do
            W=${geom%x*}; H=${geom#*x}
            p offscreen "$W" "$H" "$SECS" 16 1
            p offscreen "$W" "$H" "$SECS" 2048 1
            p offscreen "$W" "$H" "$SECS" 16 2000
        done ;;
    fence) p fence 5000; p fence 5000 ;;
    esac 2>&1 | tee -a "$out" ;;
esac
echo "# done $(date -Is)" | tee -a "$out"
flock -u 9
echo "$out"
