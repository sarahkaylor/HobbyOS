#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): vkcube fps on Venus, differential so that
# instance/device/swapchain setup is not counted:
#     fps = (N2 - N1) / (t(N2) - t(N1))
# usage: guest-vkcube.sh <label> <display> <present_mode> [WxH] [extra env...]
#   present_mode: 0 IMMEDIATE, 1 MAILBOX, 2 FIFO, 3 FIFO_RELAXED
# Prints one row: label display mode WxH n1 t1 n2 t2 fps
set -euo pipefail
. "$(dirname "$0")/guest-env.sh"
label=$1; export DISPLAY=$2; pm=$3; geom=${4:-500x500}; shift 4 || shift $#
W=${geom%x*}; H=${geom#*x}
N1=${N1:-100}; N2=${N2:-1100}
run() {
    local n=$1 t0 t1; shift
    t0=$(date +%s.%N)
    env "$@" vkcube --c "$n" --present_mode "$pm" --width "$W" --height "$H" >/tmp/vkcube.txt 2>&1 \
        || { echo "vkcube failed:" >&2; tail -3 /tmp/vkcube.txt >&2; exit 1; }
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.4f", b-a}'
}
t1=$(run "$N1" "$@")
t2=$(run "$N2" "$@")
fps=$(awk -v n1="$N1" -v n2="$N2" -v a="$t1" -v b="$t2" 'BEGIN{printf "%.2f", (n2-n1)/(b-a)}')
printf '%s %s pm=%s %s n1=%d t1=%.3f n2=%d t2=%.3f fps=%.1f\n' "$label" "$DISPLAY" "$pm" "$geom" "$N1" "$t1" "$N2" "$t2" "$fps"
