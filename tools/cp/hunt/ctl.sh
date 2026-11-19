#!/usr/bin/env bash
# tools/cp/hunt/ctl.sh RUN TAG 'line1' 'line2' ... -- drive an interactive P9
# boot (boot_p9.py `ctl`, e.g. tools/cp/hunt/settings.p9) from ssh: append the
# boot_p9 lines to obj/cp-p9/RUN/ctl.txt, then `shot TAG`, wait for that shot,
# and print its OCR (first 600 chars) and tools/cp/hunt/focusprobe.py's
# main-menu and pause-menu focus readings for it (numbers only; the shot stays
# on strix).  Lane H1-hunt round 2.
#   env: CTL_GAME_RES (default 1280x720), CTL_TIMEOUT (s, default 120), HUNT_LOGS
#   round 3: CTL_UNTIL=REGEX [CTL_UNTIL_S=300]: after the lines, keep taking
#   shots TAG-u<n> 4 s apart until one's OCR matches REGEX (case-insensitive)
#   or CTL_UNTIL_S passes -- a wait that cannot end the boot (a failed
#   `expect` in a ctl boot ends it: h3-ctl1)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RUN="$1"; TAG="$2"; shift 2
OUT="$ROOT/obj/cp-p9/$RUN"; C="$OUT/ctl.txt"
L="${HUNT_LOGS:-$ROOT/obj/hunt-logs}/$RUN.log"
[ -d "$OUT" ] || { echo "ctl: no $OUT" >&2; exit 2; }
for l in "$@"; do printf '%s\n' "$l"; done >> "$C"
printf 'shot %s\n' "$TAG" >> "$C"
end=$((SECONDS + ${CTL_TIMEOUT:-120}))
while ! grep -qa "shot s[0-9]*-t[0-9]*-$TAG:" "$L"; do
  grep -qa "verdict: " "$L" && { echo "ctl: the boot ended: $(grep -a 'verdict: ' "$L" | tail -1)"; exit 1; }
  [ $SECONDS -ge $end ] && { echo "ctl: no shot $TAG after ${CTL_TIMEOUT:-120} s"; exit 1; }
  sleep 1
done
if [ -n "${CTL_UNTIL:-}" ]; then
  uend=$((SECONDS + ${CTL_UNTIL_S:-300})); k=0
  while ! grep -a "shot s[0-9]*-t[0-9]*-$TAG[-u0-9]*:" "$L" | grep -qiE "$CTL_UNTIL"; do
    [ $SECONDS -ge $uend ] && { echo "ctl: /$CTL_UNTIL/ not seen in ${CTL_UNTIL_S:-300} s"; break; }
    k=$((k + 1)); printf 'wait 4\nshot %s-u%d\n' "$TAG" "$k" >> "$C"
    while ! grep -qa "shot s[0-9]*-t[0-9]*-$TAG-u$k:" "$L"; do
      grep -qa "verdict: " "$L" && { echo "ctl: the boot ended"; exit 1; }; sleep 1
    done
  done
  TAG="$(grep -a "shot s[0-9]*-t[0-9]*-$TAG[-u0-9]*:" "$L" | tail -1 | sed 's/.*shot s[0-9]*-t[0-9]*-\([^:]*\):.*/\1/')"
  echo "ctl: until -> $TAG after $k extra shots"
fi
grep -a "shot s[0-9]*-t[0-9]*-$TAG:" "$L" | tail -1 | cut -c1-600
f="$(ls "$OUT"/shots/s*-t*-"$TAG".png 2>/dev/null | tail -1)"
[ -n "$f" ] && for m in main pause; do
  python3 -I "$ROOT/tools/cp/hunt/focusprobe.py" --menu $m --game-res "${CTL_GAME_RES:-1280x720}" "$f" | sed "s/^/[$m] /"
done
# round 3: the save list's cyan frame too (listseek's reading)
[ -n "$f" ] && python3 -I "$ROOT/tools/cp/hunt/focusprobe.py" --list --game-res "${CTL_GAME_RES:-1280x720}" "$f" | sed "s/^/[list] /"
