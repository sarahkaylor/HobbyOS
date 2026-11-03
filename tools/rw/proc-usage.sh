#!/usr/bin/env bash
# tools/rw/proc-usage.sh — what the real RimWorld binary actually does with
# /proc/self/*, measured with strace. Drives the design of the compat /proc
# entries (M6 launch plumbing): /proc/self/exe, /proc/self/fd/N, maps.
#
# One run, one trace: every path-based syscall (open/openat/stat/readlink/
# access) plus read() annotated with -y, so a read of an fd that was opened on
# /proc/self/maps shows the path it came from.
#
#   ./proc-usage.sh [seconds]        # default 30
#
# Output: tools/rw/out/proc-usage.txt (git-ignored) with the /proc/self lines,
# and out/proc-usage-summary.txt with per-call-shape counts.
set -uo pipefail
unset LD_LIBRARY_PATH MESA_LIB LIBGL_DRIVERS_PATH GALLIUM_DRIVERS_DIRS

ROOT=$(cd "$(dirname "$0")" && pwd)
GAME=${RW_GAME:-/mnt/fast/games/steamapps/common/RimWorld}
OUT=${RW_OUT:-$ROOT/out}
SECS=${1:-30}
[ -x "$GAME/RimWorldLinux" ] || { echo "no RimWorldLinux under $GAME" >&2; exit 2; }
mkdir -p "$OUT"

DISP=${RW_DISPLAY:-:95}
n=0
while [ "$n" -lt 40 ] && [ -e "/tmp/.X${DISP#:}-lock" ]; do
  n=$((n + 1)); DISP=":$((95 + n))"
done
Xvfb "$DISP" -screen 0 1280x720x24 -nolisten tcp -ac >"$OUT/xvfb-proc.log" 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null' EXIT
for _ in $(seq 1 60); do xdpyinfo -display "$DISP" >/dev/null 2>&1 && break; sleep 0.1; done
for _ in $(seq 1 60); do
  DISPLAY="$DISP" LIBGL_ALWAYS_SOFTWARE=1 glxinfo -B >/dev/null 2>&1 && break
  sleep 0.25
done

( export DISPLAY="$DISP" HOME="$OUT/home" LC_ALL=C LANG=C
  export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=8
  exec strace -f -y -e signal=none \
       -e trace=open,openat,readlink,readlinkat,stat,lstat,newfstatat,access,read \
       -o "$OUT/strace-proc.txt" \
       "$GAME/RimWorldLinux" -logFile "$OUT/proc-usage-player.log" \
       -screen-width 1280 -screen-height 720 -screen-fullscreen 0 -force-glcore ) &
SPID=$!
sleep "$SECS"
GAMEPID=$(pgrep -x RimWorldLinux | head -1)
[ -n "$GAMEPID" ] && kill -TERM "$GAMEPID" 2>/dev/null
sleep 2
kill -TERM "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null

TRACE="$OUT/strace-proc.txt"
[ -s "$TRACE" ] || { echo "no trace ($TRACE)" >&2; exit 2; }
grep -aE '/proc/self' "$TRACE" >"$OUT/proc-usage.txt" || true
{
  echo "== /proc/self calls by syscall+shape =="
  grep -aoE '^[0-9]+ +[a-z0-9_]+\(' "$OUT/proc-usage.txt" | awk '{print $2}' | sort | uniq -c | sort -rn
  echo
  echo "== distinct /proc/self paths =="
  grep -aoE '/proc/self/[^",)>]*' "$OUT/proc-usage.txt" | sed -E 's#/proc/self/fd/[0-9]+#/proc/self/fd/N#' | sort | uniq -c | sort -rn
  echo
  echo "== first 40 lines =="
  head -40 "$OUT/proc-usage.txt"
} >"$OUT/proc-usage-summary.txt"
cat "$OUT/proc-usage-summary.txt"
echo "raw: $TRACE / $OUT/proc-usage.txt"
