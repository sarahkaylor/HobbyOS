#!/usr/bin/env bash
# tools/cp/kglitch/xrun-host-test.sh -- the kernel's ALSA code (src/kernel/
# linux_snd.c, LSND_HOST_TEST shim, as tools/cp/audio/host-alsa-test.sh) plays
# tools/cp/kglitch/xrunone.c's deliberate underrun under the staged alsa-lib.
# PASS = an "[SND] xrun n=" row is printed BEFORE the "[SND] pcm closed" row
# (H4-kglitch, cp-hunt H1-11).  Host only, no boot.
#   bash tools/cp/kglitch/xrun-host-test.sh [OUT]   Env: CP_ALSA_WORK, CP_WINE_SYSROOT
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${1:-$ROOT/obj/cp-kglitch/xrun}"
AW="${CP_ALSA_WORK:-$HOME/cp/lanes/audio-alsa}"
SYS="${CP_WINE_SYSROOT:-$HOME/cp/lanes/audio-sysroot}"
ALSA="$AW/destdir/CP/WINE"
mkdir -p "$OUT/home"
rm -rf "$OUT/conf"; cp -a "$ALSA/share/alsa" "$OUT/conf"
sed -i '/^@hooks \[/,/^\]/d' "$OUT/conf/alsa.conf"
cc -O1 -g -Wall -shared -fPIC -DLSND_HOST_TEST -iquote "$ROOT/src/host" -iquote "$ROOT/src/include" \
  "$ROOT/src/kernel/linux_snd.c" "$ROOT/src/kernel/linux_snd_params.c" "$ROOT/src/host/lsnd_host_dev.c" \
  "$ROOT/src/host/lsnd_preload.c" -o "$OUT/lsnd_preload.so" -ldl || { echo "xrun-host-test: shim build failed"; exit 2; }
clang --sysroot="$SYS" -O2 -Wall -I"$ALSA/include" -o "$OUT/xrunone" "$ROOT/tools/cp/kglitch/xrunone.c" \
  -L"$ALSA/lib" -lasound -lm || { echo "xrun-host-test: xrunone build failed"; exit 2; }
env -i PATH=/usr/bin:/bin HOME="$OUT/home" ALSA_CONFIG_DIR="$OUT/conf" ALSA_PLUGIN_DIR=/nonexistent \
  LD_LIBRARY_PATH="$ALSA/lib" LD_PRELOAD="$OUT/lsnd_preload.so" LSND_HOST_WAV="$OUT/xrun.wav" \
  timeout 60 "$OUT/xrunone" hw:0,0 > "$OUT/xrun.log" 2>&1
rc=$?
grep -aE '^\[XRUN\]|^\[SND\] (xrun|pcm closed)|ALSA lib' "$OUT/xrun.log" | sed 's/^/  /'
rm -f "$OUT/xrun.wav"
x=$(grep -an '^\[SND\] xrun n=' "$OUT/xrun.log" | head -1 | cut -d: -f1)
c=$(grep -an '^\[SND\] pcm closed' "$OUT/xrun.log" | head -1 | cut -d: -f1)
if [ "$rc" = 0 ] && [ -n "$x" ] && { [ -z "$c" ] || [ "$x" -lt "$c" ]; }; then
  echo "xrun-host-test: PASS -- the xrun was reported while the PCM was open"; exit 0
fi
echo "xrun-host-test: FAIL -- rc=$rc, no [SND] xrun row before the close row"; exit 1
