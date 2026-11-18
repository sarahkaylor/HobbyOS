#!/usr/bin/env bash
# tools/cp/audio/run-probe-snd.sh -- the snd probe WITH the opt-in virtio-sound
# device, then the host-side WAV check (lane C4-audio, docs/cp-audio.md).
#
#   bash tools/cp/qemu-slot.sh bash tools/cp/audio/run-probe-snd.sh [OUTDIR]
#
# The device's output goes to OUTDIR/probe-snd.wav through QEMU's `wav`
# audiodev (never a host audio backend: strix's sound server is the user's
# desktop).  Passes when the probe printed "[LP] done rc=0" AND the WAV holds
# >= 2.0 s of the probe's 1000 Hz tone.  The gate-2 run of the same probe
# (no device) is tools/gate/t3_linux_probe.sh snd, unchanged.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-audio}"
mkdir -p "$OUT"
WAV="$OUT/probe-snd.wav"
rm -f "$WAV"
ACCEL=""
[ -e /dev/kvm ] && ACCEL="-enable-kvm"
cd "$ROOT"
CP_AUDIO_WAV="$WAV" T3_QEMU_ARGS="$ACCEL" LP_OUT="$OUT/probe-snd.log" LP_TIMEOUT="${LP_TIMEOUT:-300}" \
  bash tools/gate/t3_linux_probe.sh snd
prc=$?
echo "run-probe-snd: probe rc=$prc"
python3 "$ROOT/tools/cp/audio/wavcheck.py" "$WAV" --freq 1000 --min-tone-s 2.0
wrc=$?
echo "run-probe-snd: wavcheck rc=$wrc"
[ $prc = 0 ] && [ $wrc = 0 ] && { echo "run-probe-snd: PASS"; exit 0; }
echo "run-probe-snd: FAIL"; exit 1
