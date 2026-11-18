#!/usr/bin/env bash
# tools/cp/audio2/pace-host-test.sh -- the kernel's ALSA code (src/kernel/
# linux_snd.c, LSND_HOST_TEST shim, as tools/cp/audio/host-alsa-test.sh) on the
# host model of QEMU's audio path (LSND_HOST_QEMU=1, src/host/lsnd_host_dev.c:
# 10 ms audio timer, 1024-frame mixing buffer, the wav backend's rate control),
# driven by tools/cp/audio2/atone.c (the winealsa model) under the staged
# alsa-lib.  Lane H5-audio2, docs/cp-audio2.md.  Host only, no boot, ~1 min.
#
#   bash tools/cp/audio2/pace-host-test.sh [OUT]   Env: CP_ALSA_WORK, CP_WINE_SYSROOT
#
# Three runs of 20 s, graded on the xruns after the stream's first second
# (the close row's xruns= minus the [SND] xrun rows with t_ms <= 1000:
# winealsa's start races the device, as on any card):
#   calm    no stall, the game fills its 40 ms buffer on every event   PASS: 0
#   stall   the same + 5 whole-process stalls of 300 ms (SIGSTOP of the test's
#           own player process: the host-stall shape of cp-hunt H1-15 and the
#           [CONS] stalls= rows; QEMU's timer thread freezes with it) PASS: <= 2
#   lowpad  no stall, the game renders one period per event (fill=1) PASS: <= 10
# Measured on strix (base = integ16 + the timing rows, two runs): calm 0-1,
# stall 12-31, lowpad 1795-1837; with the fix 0, 0-1, 1-3.
# Before the play-clock fix (docs/cp-audio2.md §4) a stall cost an xrun or a
# cascade of them (QEMU pays the stalled time back at up to 2x real time) and
# lowpad underran on most writer ticks (QEMU's mixing buffer takes a whole
# message at once and the driver counted it as played).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-audio2/host}"
AW="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}"
SYS="${CP_WINE_SYSROOT:-$HOME/cp/lanes/audio-sysroot}"
ALSA="$AW/destdir/CP/WINE"
mkdir -p "$OUT/home"
cc -O1 -g -Wall -shared -fPIC -DLSND_HOST_TEST -iquote "$ROOT/src/host" -iquote "$ROOT/src/include" \
  "$ROOT/src/kernel/linux_snd.c" "$ROOT/src/kernel/linux_snd_params.c" "$ROOT/src/host/lsnd_host_dev.c" \
  "$ROOT/src/host/lsnd_preload.c" -o "$OUT/lsnd_preload.so" -ldl || { echo "pace-host-test: shim build failed"; exit 2; }
clang --sysroot="$SYS" -O2 -Wall -I"$ALSA/include" -o "$OUT/atone" "$ROOT/tools/cp/audio2/atone.c" \
  -L"$ALSA/lib" -lasound -lm -lpthread || { echo "pace-host-test: atone build failed"; exit 2; }
rc=0
run() { # tag nstalls fill limit
  local tag=$1 ns=$2 fill=$3 lim=$4
  env -i PATH=/usr/bin:/bin HOME="$OUT/home" ALSA_PLUGIN_DIR=/nonexistent LSND_HOST_QEMU=1 \
    LD_LIBRARY_PATH="$ALSA/lib" LD_PRELOAD="$OUT/lsnd_preload.so" LSND_HOST_WAV="$OUT/$tag.wav" \
    timeout 90 "$OUT/atone" 20 fill=$fill > "$OUT/$tag.log" 2>&1 &
  local pid=$! cpid=""
  if [ "$ns" -gt 0 ]; then
    sleep 3
    cpid=$(pgrep -P $pid | head -1) # timeout's child: the player itself
    for i in $(seq 1 "$ns"); do
      [ -n "$cpid" ] && kill -STOP $cpid 2>/dev/null || break
      sleep 0.3
      kill -CONT $cpid
      sleep 3
    done
  fi
  wait $pid
  local r=$? x
  local all early
  all=$(sed -n 's/^\[SND\] pcm closed: .* xruns=\([0-9]*\) .*/\1/p' "$OUT/$tag.log" | tail -1)
  early=$(grep -aoE '^\[SND\] xrun n=.* t_ms=[0-9]+' "$OUT/$tag.log" | awk -F't_ms=' '$2 <= 1000' | wc -l)
  x=$(( ${all:-999999} - early ))
  grep -aE '^\[SND\] (stat|pcm closed)|^\[AT\] (total|done)' "$OUT/$tag.log" | sed "s/^/  $tag: /"
  python3 "$ROOT/tools/cp/hunt/wavgaps.py" "$OUT/$tag.wav" | grep -E 'dropouts=|dropout_ms_total|duration_s' | sed "s/^/  $tag: /"
  rm -f "$OUT/$tag.wav"
  if [ "$r" = 0 ] && [ "$x" -le "$lim" ]; then
    echo "pace-host-test: $tag PASS -- $x xruns after the first second ($all in all), $ns stalls"
  else
    echo "pace-host-test: $tag FAIL -- rc=$r, $x xruns after the first second ($all in all), $ns stalls"; rc=1
  fi
}
run calm 0 0 0
run stall 5 0 2
run lowpad 0 1 10
[ $rc = 0 ] && echo "pace-host-test: PASS" || echo "pace-host-test: FAIL"
exit $rc
