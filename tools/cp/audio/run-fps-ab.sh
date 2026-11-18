#!/usr/bin/env bash
# tools/cp/audio/run-fps-ab.sh -- P9 fpsprobe A/B, audio device on vs off
# (lane C4-audio, docs/cp-audio.md §7).  Same tree, same Wine (the winealsa
# stage), same Venus ICD; the only difference is CP_AUDIO_WAV (on: the game
# finds the virtio-snd card and Wwise plays; off: no card, null audio).
# Boots alternate on/off, N per arm (default 3).  Each is a 32 GiB P9 boot:
# run-p9.sh / boot_p9.py take /tmp/cp-bigguest.lock inside the QEMU slot.
# The WAVs are game content: graded (level, active seconds) then deleted.
#
#   bash tools/cp/audio/run-fps-ab.sh [N]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
N="${1:-3}"
D="$ROOT/obj/cp-audio/fps"; mkdir -p "$D"
cd "$ROOT"
for i in $(seq 1 "$N"); do
  for arm in on off; do
    R="audio-fps-$arm$i"
    WAV=""; [ $arm = on ] && WAV="$D/$R.wav"
    rm -f "$D/$R.wav"
    env ${WAV:+CP_AUDIO_WAV=$WAV} CP_WINE_STAGE="$HOME/cp/lanes/audio-wine/stage/CP/WINE" \
      P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN="$R" VENUS_BUILD="$HOME/cp/lanes/audio-venus" \
      P9_P8_SEEDED="$HOME/cp/lanes/audio-p8/pfx-seeded.img" P8_EXPORT=/srv/nfs/cp-audio-p9 \
      PATH="$HOME/.local/bin:$PATH" MAKEFLAGS=-j16 TOOLS_SKIP=1 \
      bash tools/cp/p9/run-p9.sh > "$D/$R.log" 2>&1
    echo "$R rc=$? $(date +%T)" | tee -a "$D/ab.txt"
    grep -aE "world|menu|phase" "$D/$R.log" | grep -aiE "fps|median" | tail -6 | sed "s/^/  /" | tee -a "$D/ab.txt"
    if [ -n "$WAV" ]; then
      python3 tools/cp/audio/wavcheck.py "$WAV" | sed "s/^/  /" | tee -a "$D/ab.txt"
      rm -f "$WAV"
    fi
    rm -f "$ROOT/obj/cp-p9/$R/cpdata.img" "$ROOT/obj/cp-p9/$R/disk.img"
  done
done
echo "ALLDONE $(date +%T)" >> "$D/ab.txt"
