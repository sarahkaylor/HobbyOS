#!/usr/bin/env bash
# tools/cp/p9/run-p9.sh -- Gate P9 game boots on Venus (lane C9-play,
# docs/cp-p9.md): Cyberpunk 2077 in HobbyOS, the user's own save loaded and
# played with scripted QMP input, then saved, quit, rebooted and reloaded.
#
# Built on the P8 harness, not beside it: tools/cp/p8/run-p8.sh does the build,
# the staging export, the REFCOUNTED game-copy export (/tmp/cp-game-export.lock,
# never exports.sh on /srv/nfs/cp-game directly), the prefix image and the
# game-copy census; this script only
#   1. makes obj/cp-p9/pfx-saves.img once: a copy of P8's seeded prefix
#      (obj/cp-p8/pfx-seeded.img) plus the user's saves (seed-saves.sh,
#      sha256 read back; e2fsck -fn before and after);
#   2. runs run-p8.sh with P8_GPU=venus, the P9 boot driver
#      (tools/cp/p9/boot_p9.py: play script, session record, big guest),
#      P8_MEM (default 32768: the user's approval for P9 game boots, one at a
#      time under /tmp/cp-bigguest.lock inside the QEMU slot) and, for the
#      reload boot, the previous boot's data disk (P9_FROM=<run>);
#   3. summarizes the run: fps per phase, frame-pool use, the saves now on the
#      image (names, sizes, mtimes, and the game's own metadata playerPosition
#      of saves this run created -- never their contents otherwise).
#
#   P9_SCRIPT   play script (default tools/cp/p9/play.p9)
#   P9_FROM     run name under obj/cp-p9 whose cpdata.img this boot starts from
#   P9_SAVES    saves to seed (default "ManualSave-86 AutoSave-16")
#   P9_XSCREEN  XHOBBY's startup screen (default 1280x720: the user's 1280x720
#               borderless window then covers the screen and C6-p7's zero-copy
#               scanout presents it; "" = the 1920x1080 framebuffer size)
#   P9_GAME_RES the game's own Resolution setting (default 1280x720; 1920x1080
#               with P9_XSCREEN=1920x1080: boot_p9 scales its shots to the
#               1280x720 menu tables, cp-hunt H1-08)
#   P9_SCANOUT  1 (default) or 0: HB_SCANOUT=0 in the game's environment
#               (put_image only, the A/B)
#   VENUS_BUILD the guest Venus Mesa (default ~/cp/lanes/play-venus: this
#               tree's tools/mesa/venus/build.sh with C6-p7's WSI)
#   P9_MEM      guest MiB (default 32768, max 32768); P9_GAME_S (launcher's game
#               deadline, default 3000); TIMEOUT_S (default 5400); P9_RUN.
# First run once: P8_PHASE=pfx bash tools/cp/p8/run-p8.sh (the prefix image).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
export PATH="/usr/sbin:/sbin:$PATH"
die() { echo "run-p9: $*" >&2; exit 2; }
PROF="${P8_PROFILE:-$HOME/cp/game-profile}"
B9="$ROOT/obj/cp-p9"
P8SEED="${P9_P8_SEEDED:-$ROOT/obj/cp-p8/pfx-seeded.img}"
SAVES_IMG="$B9/pfx-saves.img"
SAVES="${P9_SAVES:-ManualSave-86 AutoSave-16}"
SCRIPT="${P9_SCRIPT:-tools/cp/p9/play.p9}"
[ -f "$SCRIPT" ] || die "no play script $SCRIPT"
mkdir -p "$B9"

if [ ! -f "$SAVES_IMG" ]; then
  [ -f "$P8SEED" ] || die "no $P8SEED: run P8_PHASE=pfx bash tools/cp/p8/run-p8.sh first"
  T="$B9/pfx-saves.img.tmp"
  cp --sparse=always "$P8SEED" "$T"
  e2fsck -fn "$T" > "$B9/fsck-pre-saves.log" 2>&1 || die "e2fsck of $P8SEED copy: $(tail -2 "$B9/fsck-pre-saves.log")"
  # shellcheck disable=SC2086
  bash tools/cp/p9/seed-saves.sh "$T" "$PROF" "$B9/seed-saves.log" $SAVES || die "seed-saves"
  e2fsck -fn "$T" > "$B9/fsck-saves.log" 2>&1 || die "e2fsck after seeding saves: $(tail -3 "$B9/fsck-saves.log")"
  echo "== e2fsck -fn after seeding the saves: rc=0 ($(tail -1 "$B9/fsck-saves.log"))"
  mv "$T" "$SAVES_IMG"
  echo "== prefix with saves: $SAVES_IMG"
fi

FROM_IMG=""
if [ -n "${P9_FROM:-}" ]; then
  FROM_IMG="$B9/$P9_FROM/cpdata.img"
  [ -f "$FROM_IMG" ] || die "no $FROM_IMG"
fi
if [ -n "${P9_RUN:-}" ]; then RUN="$P9_RUN"; else n=1; while [ -e "$B9/run$n" ]; do n=$((n+1)); done; RUN="run$n"; fi
MEM="${P9_MEM:-32768}"
XSCREEN="${P9_XSCREEN-1280x720}"
BARGS="--script $ROOT/$SCRIPT --play-shot-every ${P9_SHOT_S:-30} --xscreen ${XSCREEN:-1920x1080}"
# cp-hunt H1-08: the game's own Resolution (prep-settings.sh Resolution=...);
# boot_p9 scales a 1920x1080 game's shots to the 1280x720 row tables
BARGS="$BARGS --game-res ${P9_GAME_RES:-1280x720}"
P8CF="${P8_CFLAGS:-}"
if [ "${P9_SCANOUT:-1}" = 0 ]; then P8CF="$P8CF -DP8_EXTRA_ENV=\"HB_SCANOUT=0\""; fi
export VENUS_BUILD="${VENUS_BUILD:-$HOME/cp/lanes/play-venus}"
# the glass, not the X framebuffer: a scanout blob is only visible over VNC
# (docs/cp-p7.md §4; tools/cp/venus/p5venus.py prefers it when the socket exists)
export VENUS_VNC="${VENUS_VNC:-1}"
[ "$MEM" -gt 12288 ] && BARGS="$BARGS --big-guest"
echo "== run-p9 run=$RUN script=$SCRIPT mem=${MEM}M from=${P9_FROM:-<fresh: pfx-saves.img>} xscreen=${XSCREEN:-fb} scanout=${P9_SCANOUT:-1} venus=$VENUS_BUILD vnc=$VENUS_VNC $(date -Is)"

P8_GPU=venus P8_BASE="$B9" P8_SEEDED="$SAVES_IMG" P8_CPDATA_FROM="$FROM_IMG" \
P8_BOOT=tools/cp/p9/boot_p9.py P8_BOOT_ARGS="$BARGS" P8_MEM="$MEM" P8_RUN="$RUN" \
P8_GAME_S="${P9_GAME_S:-3000}" TIMEOUT_S="${TIMEOUT_S:-5400}" P8_STALL_S="${P8_STALL_S:-900}" \
P8_XSCREEN="$XSCREEN" P8_CFLAGS="$P8CF" P8_EXPORT="${P8_EXPORT:-/srv/nfs/cp-play-p9}" P8_WINEDEBUG="${P8_WINEDEBUG:-+timestamp,+loaddll,fixme-all,+fps,+process}" \
  bash tools/cp/p8/run-p8.sh
rc=$?
OUT="$B9/$RUN"
[ -f "$OUT/fps.csv" ] && python3 -I tools/cp/p9/summarize.py "$OUT" | tee "$OUT/summary.txt"
if [ -f "$OUT/cpdata.img" ]; then
  echo "== saves on the data disk after the boot (names, sizes, mtimes)"
  bash tools/cp/p9/saves.sh "$OUT/cpdata.img" | tee "$OUT/saves-after.txt"
fi
exit $rc
