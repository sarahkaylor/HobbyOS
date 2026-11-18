#!/usr/bin/env bash
# tools/cp/audio/run-game-audio.sh -- one P8 game boot to the main menu with the
# opt-in virtio-sound device, Wine = the winealsa build (lane C4-audio,
# docs/cp-audio.md §7).  The WAV is GAME CONTENT: it is measured here and
# deleted; only the numbers are kept (never commit it).
#
#   bash tools/cp/audio/run-game-audio.sh [RUN]     (RUN default audio-venus-on)
# Env: P8_GPU (default venus), VENUS_BUILD (default ~/cp/lanes/audio-venus),
#      CP_WINE_STAGE (default the winealsa stage), P8_SEEDED, P8_EXPORT,
#      KEEP_WAV=1 (debug only: keep the WAV; still never commit it)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RUN="${1:-audio-venus-on}"
D="$ROOT/obj/cp-audio/game"; mkdir -p "$D"
WAV="$D/$RUN.wav"; rm -f "$WAV"
cd "$ROOT"
CP_AUDIO_WAV="$WAV" CP_WINE_STAGE="${CP_WINE_STAGE:-$HOME/cp/lanes/audio-wine/stage/CP/WINE}" \
P8_GPU="${P8_GPU:-venus}" VENUS_BUILD="${VENUS_BUILD:-$HOME/cp/lanes/audio-venus}" VENUS_VNC=1 \
P8_SEEDED="${P8_SEEDED:-$HOME/cp/lanes/audio-p8/pfx-seeded.img}" P8_EXPORT="${P8_EXPORT:-/srv/nfs/cp-audio-p8g}" \
P8_RUN="$RUN" TIMEOUT_S="${TIMEOUT_S:-3600}" P8_STALL_S=600 P8_GAME_S="${P8_GAME_S:-3000}" \
MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP=1 PATH="$HOME/.local/bin:$PATH" \
  bash tools/cp/p8/run-p8.sh > "$D/$RUN.p8.log" 2>&1
prc=$?
grep -aiE "main menu OCR|verdict|end game" "$D/$RUN.p8.log" | tail -4
echo "run-game-audio: p8 rc=$prc"
python3 tools/cp/audio/wavcheck.py "$WAV" --min-active-s 10 > "$D/$RUN.wavcheck.txt"
wrc=$?
cat "$D/$RUN.wavcheck.txt"
S="$ROOT/obj/cp-p8/$RUN/serial.txt"
grep -a '\[SND\] \(hw_params\|pcm closed\|HW_PARAMS\)' "$S" 2>/dev/null | sort | uniq -c | sort -rn | head -8 > "$D/$RUN.snd-rows.txt"
cat "$D/$RUN.snd-rows.txt"
[ "${KEEP_WAV:-0}" = 1 ] || { rm -f "$WAV"; echo "run-game-audio: WAV deleted (game content)"; }
rm -f "$ROOT/obj/cp-p8/$RUN/cpdata.img" "$ROOT/obj/cp-p8/$RUN/disk.img"
[ $prc = 0 ] && [ $wrc = 0 ] && { echo "run-game-audio: PASS"; exit 0; }
echo "run-game-audio: FAIL"; exit 1
