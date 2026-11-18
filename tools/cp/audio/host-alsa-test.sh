#!/usr/bin/env bash
# tools/cp/audio/host-alsa-test.sh -- the kernel's ALSA code under an
# UNMODIFIED alsa-lib, on the host, no boot (lane C4-audio, docs/cp-audio.md §5).
#
# src/kernel/linux_snd.c + linux_snd_params.c are built with LSND_HOST_TEST
# into an LD_PRELOAD shim (src/host/lsnd_preload.c) with a host model of the
# virtio-sound device (src/host/lsnd_host_dev.c: QEMU's measured behaviour,
# real-time pacing, completed bytes appended to a WAV).  Every open under
# /dev/snd and /dev/aload is intercepted, so the host's real sound devices are
# never touched; ALSA_PLUGIN_DIR points nowhere, so no external plugin (a
# PipeWire bridge) can load.  Then tools/cp/audio/alsatone plays through
# plughw:0,0 in S16_LE and in FLOAT_LE and wavcheck.py grades the WAV.
#
#   bash tools/cp/audio/host-alsa-test.sh [OUT]
# Env: CP_ALSA_WORK (default ~/cp/lanes/audio-alsa)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-audio/host}"
AW="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}"
ALSA="$AW/destdir/CP/WINE"
mkdir -p "$OUT/home"
# Hermetic config: the staged alsa config with alsa.conf's top-level @hooks
# (which load /etc/alsa/conf.d, /etc/asound.conf, ~/.asoundrc: the HOST's
# PipeWire defaults) removed, so the host resolves names exactly as the guest.
rm -rf "$OUT/conf"; cp -a "$ALSA/share/alsa" "$OUT/conf"
sed -i '/^@hooks \[/,/^\]/d' "$OUT/conf/alsa.conf"
cc -O1 -g -Wall -shared -fPIC -DLSND_HOST_TEST -iquote "$ROOT/src/host" -iquote "$ROOT/src/include" \
  "$ROOT/src/kernel/linux_snd.c" "$ROOT/src/kernel/linux_snd_params.c" "$ROOT/src/host/lsnd_host_dev.c" \
  "$ROOT/src/host/lsnd_preload.c" -o "$OUT/lsnd_preload.so" -ldl || { echo "host-alsa-test: shim build failed"; exit 2; }
rc=0
run() { # tag dev hz ms fmt [rate]
  local tag=$1; shift
  rm -f "$OUT/$tag.wav"
  env -i PATH=/usr/bin:/bin HOME="$OUT/home" ALSA_CONFIG_DIR="$OUT/conf" ALSA_PLUGIN_DIR=/nonexistent \
    LD_LIBRARY_PATH="$ALSA/lib" LD_PRELOAD="$OUT/lsnd_preload.so" LSND_HOST_WAV="$OUT/$tag.wav" \
    timeout 60 "$AW/alsatone" "$@" > "$OUT/$tag.log" 2>&1
  local r=$?
  grep -E '^\[ALSA\] (done|wrote|buffer)|^\[SND\] (hw_params|pcm closed|HW_PARAMS)|ALSA lib' "$OUT/$tag.log" | sed "s/^/  $tag: /"
  python3 "$ROOT/tools/cp/audio/wavcheck.py" "$OUT/$tag.wav" --freq "$2" --min-tone-s "$(awk "BEGIN{print $3/1000*0.8}")" > "$OUT/$tag.wavcheck"
  local w=$?
  sed "s/^/  $tag: /" "$OUT/$tag.wavcheck"
  echo "host-alsa-test: $tag ($*) alsatone rc=$r wavcheck rc=$w"
  [ "$r" = 0 ] && [ "$w" = 0 ] || rc=1
}
run s16      plughw:0,0 660 1000 s16
run float    plughw:0,0 770 1000 float
run hw       hw:0,0     440 1000 s16
run default  default    550 1000 float
run resample default    880 1000 s16 44100
[ $rc = 0 ] && echo "host-alsa-test: PASS" || echo "host-alsa-test: FAIL"
exit $rc
