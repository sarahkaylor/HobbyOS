#!/usr/bin/env bash
# tools/cp/hunt/run-hunt.sh KIND RUN -- one boot of the H1-hunt matrix (wave 8,
# docs/cp-hunt.md): the existing P8 / P9 harnesses, unchanged, plus
#   * a host sampler for the whole boot (1 Hz: the 9070 XT's gpu_busy_percent
#     on renderD129, loadavg; the other QEMU guests at the start and every 60 s)
#     -> $HUNT_LOGS/RUN.host.txt  (the C1-k7 k7-p9.sh sampler, whole-boot);
#   * the log classifier (tools/cp/hunt/classify.py) on the serial log
#     -> $HUNT_LOGS/RUN.class.txt (+ .json);
#   * with HUNT_AUDIO=1, CP_AUDIO_WAV under ~/cp/lanes/hunt-wav and the gap /
#     drift check (tools/cp/hunt/wavgaps.py) -> RUN.wav.txt; the WAV is then
#     deleted (game content).
#
# KIND:
#   p8v       P8 to the main menu, Venus (12 GiB)
#   p8l       P8 to the main menu, lavapipe (12 GiB)
#   win       P8 Venus on a data image with WindowMode=Windowed (prep-windowed.sh)
#   fp720     P9 fpsprobe.p9, 1280x720 Low (32 GiB, big-guest lock)
#   fp1080    P9 fpsprobe.p9 at 1920x1080 Low (data image from prep-settings.sh,
#             X screen 1920x1080, P9_GAME_RES=1920x1080)
#   play      P9 play.p9 (load, ~11 min of play, quick save, quit)
#   reload    P9 reload.p9 on HUNT_FROM=<play run>'s data disk
#   reload2   P9 tools/cp/hunt/reload2.p9 on HUNT_FROM=<play run>'s data disk
#             (CONTINUE from the fresh main menu: no sub-menu first, cp-hunt H1-12)
#   soak      P9 tools/cp/hunt/soak.p9 (~28 min in the world)
#   settings  P9 tools/cp/hunt/settings.p9 (Settings menu: change + apply a graphics option)
#   script    P9 with HUNT_SCRIPT=<path>
#
# Env: HUNT_AUDIO=1, HUNT_FROM (P9 run under obj/cp-p9 to start from),
#      HUNT_LOGS (default ~/cp/lanes/hunt-logs), anything run-p8/run-p9 take.
# One boot of this lane at a time (flock /tmp/cp-hunt-lane.lock); the QEMU
# slot and the big-guest lock are taken inside the harnesses, for the boot only.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
KIND="${1:?kind}"; RUN="${2:?run name}"
LOGS="${HUNT_LOGS:-$HOME/cp/lanes/hunt-logs}"; mkdir -p "$LOGS"
L="$LOGS/$RUN.log"; H="$LOGS/$RUN.host.txt"
export PATH="$HOME/.local/bin:$PATH" MAKEFLAGS=-j16 TOOLS_SKIP=1
# wave 8b: the patched Wine stage (cp-winefix) is the default
export CP_WINE_STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-cp2/stage/CP/WINE}" WINE_BUILD="${WINE_BUILD:-$HOME/cp/wine-cp2}"
export VENUS_BUILD="${VENUS_BUILD:-$HOME/cp/lanes/hunt-venus}" VENUS_VNC=1
exec 196>>/tmp/cp-hunt-lane.lock
flock 196

WAV=""
if [ "${HUNT_AUDIO:-0}" = 1 ]; then
  mkdir -p "$HOME/cp/lanes/hunt-wav"; WAV="$HOME/cp/lanes/hunt-wav/$RUN.wav"; rm -f "$WAV"
  export CP_AUDIO_WAV="$WAV"
fi

sampler() {
  echo "# run-hunt host sampler: S HH:MM:SS gpu_busy_percent(renderD129) loadavg1 serial_bytes dirty_kB writeback_kB io_full10 (PSI); G lines = other QEMU guests"
  local i=0
  while kill -0 "$1" 2>/dev/null; do
    if [ $((i % 60)) = 0 ]; then
      for p in $(pgrep -f qemu-system-x86); do
        echo "G $(date +%T) $p $(readlink /proc/$p/cwd) $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -o -- '-m [0-9]*M\|virtio-gpu[a-z-]*\|virtio-sound[a-z-]*' | tr '\n' ' ')"
      done
    fi
    # + the boot's serial.log size (a silent console = a stalled guest or a
    # stalled QEMU main loop) and the host's Dirty/Writeback kB (a QEMU
    # write() to serial.log throttled by dirty-page writeback stalls the guest)
    echo "S $(date +%T) $(cat /sys/class/drm/renderD129/device/gpu_busy_percent 2>/dev/null) $(cut -d' ' -f1 /proc/loadavg)" \
      "$(stat -c %s "$2/serial.log" 2>/dev/null || echo -)" "$(awk '/^(Dirty|Writeback):/ {printf "%s ", $2}' /proc/meminfo)" \
      "$(awk '/^full/ {sub("avg10=", "", $2); printf "io_full10=%s", $2}' /proc/pressure/io 2>/dev/null)"
    i=$((i + 1)); sleep 1
  done
}

P9=0
case "$KIND" in
  p8v)  set -- env P8_GPU=venus P8_RUN="$RUN" P8_EXPORT=/srv/nfs/cp-hunt-p8 TIMEOUT_S="${TIMEOUT_S:-3600}" \
          P8_STALL_S="${P8_STALL_S:-600}" P8_GAME_S="${P8_GAME_S:-3000}" bash tools/cp/p8/run-p8.sh ;;
  p8l)  set -- env P8_RUN="$RUN" P8_EXPORT=/srv/nfs/cp-hunt-p8 TIMEOUT_S="${TIMEOUT_S:-5400}" \
          P8_STALL_S="${P8_STALL_S:-600}" P8_GAME_S="${P8_GAME_S:-5000}" bash tools/cp/p8/run-p8.sh ;;
  win)  IMG="$ROOT/obj/cp-hunt/cpdata-windowed.img"
        [ -f "$IMG" ] || bash tools/cp/hunt/prep-settings.sh "$ROOT/obj/cp-p8/pfx-seeded.img" "$IMG" WindowMode=Windowed:0 >&2 || exit 2
        set -- env P8_GPU=venus P8_RUN="$RUN" P8_EXPORT=/srv/nfs/cp-hunt-p8 P8_CPDATA_FROM="$IMG" TIMEOUT_S="${TIMEOUT_S:-3600}" \
          P8_STALL_S="${P8_STALL_S:-600}" P8_GAME_S="${P8_GAME_S:-3000}" bash tools/cp/p8/run-p8.sh ;;
  fp720|fp1080|play|reload|reload2|soak|settings|script)
        P9=1
        case "$KIND" in
          fp720) S=tools/cp/p9/fpsprobe.p9 ;;
          # integ17: boot_p9 --game-res (P9_GAME_RES) maps 1080p shots, so the
          # stock fpsprobe.p9; HUNT_SCRIPT=tools/cp/hunt/fpsprobe-blind.p9 is the old workaround
          fp1080) S="${HUNT_SCRIPT:-tools/cp/p9/fpsprobe.p9}" ;;
          reload2) S=tools/cp/hunt/reload2.p9 ;;
          play) S=tools/cp/p9/play.p9 ;;
          reload) S=tools/cp/p9/reload.p9 ;;
          soak) S=tools/cp/hunt/soak.p9 ;;
          settings) S=tools/cp/hunt/settings.p9 ;;
          script) S="${HUNT_SCRIPT:?HUNT_SCRIPT}" ;;
        esac
        X=(P9_SCRIPT="$S" P9_RUN="$RUN" P8_EXPORT=/srv/nfs/cp-hunt-p9)
        if [ "$KIND" = fp1080 ]; then
          # a P9 "from" run whose data disk says Resolution=1920x1080 (prep-settings.sh)
          SRC="$ROOT/obj/cp-p9/hunt-src1080"
          if [ ! -f "$SRC/cpdata.img" ]; then
            [ -f "$ROOT/obj/cp-p9/pfx-saves.img" ] || { echo "run-hunt: fp1080 needs obj/cp-p9/pfx-saves.img (run one fp720 first)" >&2; exit 2; }
            mkdir -p "$SRC"
            bash tools/cp/hunt/prep-settings.sh "$ROOT/obj/cp-p9/pfx-saves.img" "$SRC/cpdata.img" Resolution=1920x1080 >&2 || exit 2
          fi
          X+=(P9_FROM=hunt-src1080 P9_XSCREEN=1920x1080 P9_GAME_RES=1920x1080)
        fi
        [ -n "${HUNT_FROM:-}" ] && X+=(P9_FROM="$HUNT_FROM")
        set -- env "${X[@]}" bash tools/cp/p9/run-p9.sh ;;
  *) echo "run-hunt: unknown kind $KIND" >&2; exit 2 ;;
esac

if [ "$P9" = 1 ]; then OUT="$ROOT/obj/cp-p9/$RUN"; else OUT="$ROOT/obj/cp-p8/$RUN"; fi
echo "== run-hunt kind=$KIND run=$RUN audio=${WAV:-off} venus=$VENUS_BUILD wine=$CP_WINE_STAGE start=$(date -Is)" > "$L"
"$@" >> "$L" 2>&1 &
BP=$!
sampler "$BP" "$OUT" > "$H" 2>&1 &
SP=$!
wait "$BP"; rc=$?
kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
echo "rc=$rc $(date +%T)" >> "$L"

SER="$OUT/serial.txt"; [ -f "$SER" ] || SER="$OUT/serial.log"
python3 -I tools/cp/hunt/classify.py --run "$RUN" --log "$L" --host "$H" --json "$LOGS/$RUN.class.json" "$SER" > "$LOGS/$RUN.class.txt" 2>&1
if [ -n "$WAV" ]; then
  python3 -I tools/cp/hunt/wavgaps.py --log "$L" "$WAV" > "$LOGS/$RUN.wav.txt" 2>&1
  python3 -I tools/cp/audio/wavcheck.py "$WAV" >> "$LOGS/$RUN.wav.txt" 2>&1
  rm -f "$WAV"
fi
# keep the evidence, drop the 16 GiB sparse data disk unless the next boot needs it (play -> reload)
cp -f "$SER" "$LOGS/$RUN.serial.txt" 2>/dev/null
for f in fps.csv mem.csv thrs.csv phases.txt summary.txt hbp.csv verdict.txt saves-after.txt; do
  [ -f "$OUT/$f" ] && cp -f "$OUT/$f" "$LOGS/$RUN.$f"
done
if [ "$KIND" != play ] && [ "${HUNT_KEEP:-0}" != 1 ]; then rm -f "$OUT/cpdata.img" "$OUT/disk.img"; fi
echo "run-hunt: $RUN rc=$rc verdict=$(cat "$OUT/verdict.txt" 2>/dev/null)"
exit $rc
