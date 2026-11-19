#!/usr/bin/env bash
# tools/cp/audio2/leadin-model-ab.sh -- winealsa lead-in variants on the host
# model (lane H5-audio2 round 2, docs/cp-audio2.md §9): atone.c (the winealsa
# model) under the kernel's linux_snd.c in the LSND_HOST_TEST shim on the
# LSND_HOST_QEMU=1 model of QEMU's audio path.  No boot, no slot.
#
#   bash tools/cp/audio2/leadin-model-ab.sh [OUT] [SECS] [RUNS]
#
# Variants:  orig  Wine 11.0 (played = min(data_in_alsa, avail); lead-in when
#                  data_in_alsa == 0 && held < period)
#            fix   played = data_in_alsa minus what ALSA holds (round 1's
#                  candidate, not recommended)
#            gateN orig accounting, lead-in only when the play clock says
#                  fewer than N periods are still queued (wine-candidates/0002)
# Client shapes: A fill=1 skip=8, B fill=2 skip=4, C fill=0 (keeps its
# buffer full), D fill=1 (one period per event).
# Per run: kernel xruns (close row), [SND] zeros runs after the stream start,
# WAV dropouts (wavgaps.py), the player's xrun recoveries and lead-ins.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-audio2/leadin}"; SECS="${2:-15}"; RUNS="${3:-2}"
AW="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}"
SYS="${CP_WINE_SYSROOT:-$HOME/cp/lanes/audio-sysroot}"
ALSA="$AW/destdir/CP/WINE"
mkdir -p "$OUT/home"
cc -O1 -g -Wall -shared -fPIC -DLSND_HOST_TEST -iquote "$ROOT/src/host" -iquote "$ROOT/src/include" \
  "$ROOT/src/kernel/linux_snd.c" "$ROOT/src/kernel/linux_snd_params.c" "$ROOT/src/host/lsnd_host_dev.c" \
  "$ROOT/src/host/lsnd_preload.c" -o "$OUT/lsnd_preload.so" -ldl || exit 2
clang --sysroot="$SYS" -O2 -Wall -I"$ALSA/include" -o "$OUT/atone" "$ROOT/tools/cp/audio2/atone.c" \
  -L"$ALSA/lib" -lasound -lm -lpthread || exit 2
for shape in "A:fill=1 skip=8" "B:fill=2 skip=4" "C:fill=0" "D:fill=1"; do
  sn=${shape%%:*}; sa=${shape#*:}
  for v in orig fix gate2 gate3; do
    case $v in orig) va="acct=orig";; fix) va="acct=fix";; gate*) va="acct=orig gate=${v#gate}";; esac
    for i in $(seq 1 "$RUNS"); do
      t="$sn-$v-$i"
      env -i PATH=/usr/bin:/bin HOME="$OUT/home" ALSA_PLUGIN_DIR=/nonexistent LSND_HOST_QEMU=1 \
        LD_LIBRARY_PATH="$ALSA/lib" LD_PRELOAD="$OUT/lsnd_preload.so" LSND_HOST_WAV="$OUT/$t.wav" \
        timeout $((SECS * 3 + 20)) "$OUT/atone" "$SECS" $sa $va > "$OUT/$t.log" 2>&1
      kx=$(sed -n 's/^\[SND\] pcm closed: .* xruns=\([0-9]*\) .*/\1/p' "$OUT/$t.log" | tail -n 1)
      zr=$(grep -aoE '^\[SND\] zeros n=[0-9]+ t_ms=[0-9]+' "$OUT/$t.log" | awk -F't_ms=' '$2 > 50' | wc -l)
      at=$(grep -aoE '\[AT\] total .*' "$OUT/$t.log" | grep -oE 'xrun_state=[0-9]+|leadin=[0-9]+|gated=[0-9]+' | tr '\n' ' ')
      wg=$(python3 "$ROOT/tools/cp/hunt/wavgaps.py" "$OUT/$t.wav" 2>/dev/null | sed -n 's/^wavgaps: dropouts=//p')
      rm -f "$OUT/$t.wav"
      echo "$sn $v run$i: kernel_xruns=${kx:-TIMEOUT} zero_runs=$zr wav_dropouts=${wg:-?} $at"
    done
  done
done
