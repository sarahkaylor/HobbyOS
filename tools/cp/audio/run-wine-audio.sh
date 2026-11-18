#!/usr/bin/env bash
# tools/cp/audio/run-wine-audio.sh -- alsa-lib + Wine audio in the guest, graded
# in the host WAV (lane C4-audio, docs/cp-audio.md §5-§6).
#
# One P5-harness boot (tools/cp/p5/run-wine-p5.sh, probe tag "audio") with the
# opt-in virtio-sound device writing $OUT/wine-audio.wav, Wine = the winealsa
# build (tools/cp/audio/build-wine-alsa.sh).  The guest plays, in order:
#   alsatone default 660 Hz 1.5 s   (alsa-lib 1.2.14, plug -> hw:0,0)
#   audiotone.exe wasapi 880 Hz 2 s (mmdevapi/winealsa.drv, IAudioClient)
#   audiotone.exe waveout 550 Hz 1.5 s (winmm -> winealsa)
# PASS = the P5 grader's audio rows + each tone present in the WAV (>= 80 % of
# its duration within 2 % of its frequency).
#
#   bash tools/cp/audio/run-wine-audio.sh [OUT]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-audio/wine}"
mkdir -p "$OUT"
WAV="$OUT/wine-audio.wav"
rm -f "$WAV"
cd "$ROOT"
CP_AUDIO_WAV="$WAV" P5_OUT="$OUT" P5_PROBES=audio \
CP_WINE_STAGE="${CP_WINE_STAGE:-$HOME/cp/lanes/audio-wine/stage/CP/WINE}" \
WINE_BUILD="${WINE_BUILD:-$HOME/cp/wine-build-x}" CP_VK_WORK="${CP_VK_WORK:-$HOME/cp/vk2-build}" \
CP_X11_WORK="${CP_X11_WORK:-$HOME/cp/x11-build-x2}" CP_P5_PE="${CP_P5_PE:-$HOME/cp/vk2-pe}" \
P5_EXPORT="${P5_EXPORT:-/srv/nfs/cp-audio-p5}" \
P5_XHOBBY="${P5_XHOBBY:-$HOME/cp/vk2-scratch/x2-xserver/XHOBBY.BIN}" \
P5_XHOBBY_SHA256="${P5_XHOBBY_SHA256:-935c760fd450eb9a3326e75691a9b8feaacf155f19033e807f98a4bf78424c97}" \
P5_XSESSION_ENV="${P5_XSESSION_ENV:-HB_SECOND_SCREEN=0}" PATH="$HOME/.local/bin:$PATH" \
MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP=1 TIMEOUT_S="${TIMEOUT_S:-2400}" \
  bash tools/cp/p5/run-wine-p5.sh > "$OUT/p5.log" 2>&1
prc=$?
grep -aE '\[P5W\] (audio|FAIL|PASS|VERDICT)' "$OUT/p5.log" | tail -8
echo "run-wine-audio: p5 rc=$prc"
rc=$prc
for spec in "660 1.2 alsatone" "880 1.6 wasapi" "550 1.2 waveout"; do
  set -- $spec
  python3 tools/cp/audio/wavcheck.py "$WAV" --freq "$1" --min-tone-s "$2" > "$OUT/wavcheck-$3.txt"
  r=$?; echo "run-wine-audio: $3 ($1 Hz, >= $2 s): $(cat "$OUT/wavcheck-$3.txt")"
  [ $r = 0 ] || rc=1
done
[ $rc = 0 ] && echo "run-wine-audio: PASS" || echo "run-wine-audio: FAIL"
exit $rc
