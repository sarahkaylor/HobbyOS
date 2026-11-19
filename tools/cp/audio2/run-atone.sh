#!/usr/bin/env bash
# tools/cp/audio2/run-atone.sh -- one boot of the cheap audio repro (lane
# H5-audio2, docs/cp-audio2.md): a test-mode wave of ATONE.ELF (+ ALOAD.ELF
# for guest load) with the opt-in virtio-sound device writing a WAV through
# QEMU's `wav` audiodev (never a host sound backend), then
# tools/cp/hunt/wavgaps.py on the WAV and the kernel's [SND] rows; the WAV is
# deleted after the analysis.
#
#   bash tools/cp/qemu-slot.sh bash tools/cp/audio2/run-atone.sh RUN SECS "ATONE ARGS" ["ALOAD ARGS"]
#
# e.g.  run-atone.sh idle1 120 ""               (tone alone)
#       run-atone.sh load1 120 "" "threads=8 mode=mix"
#       run-atone.sh stall1 120 "stall_ms=60 stall_every=10"
# Output: $OUT/RUN.console.log, $OUT/RUN.summary.txt (OUT default obj/cp-audio2).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RUN="${1:?run name}"; SECS="${2:?seconds}"; AARGS="${3:-}"; LARGS="${4:-}"
OUT="${AUDIO2_OUT:-$ROOT/obj/cp-audio2}"
WORK="${CP_AUDIO2_WORK:-$HOME/cp/lanes/audio2-alsa}"
WAVDIR="${AUDIO2_WAVDIR:-$HOME/cp/lanes/audio2-wav}"
mkdir -p "$OUT" "$WAVDIR"
WAV="$WAVDIR/$RUN.wav"; rm -f "$WAV"
[ -f "$WORK/ATONE.ELF" ] || { echo "run-atone: no $WORK/ATONE.ELF (tools/cp/audio2/build.sh)"; exit 2; }
CFG="$OUT/$RUN.wave.cfg"
printf 'ATONE.ELF %s %s\n' "$SECS" "$AARGS" > "$CFG"
[ -n "$LARGS" ] && printf 'ALOAD.ELF %s %s\n' "$SECS" "$LARGS" >> "$CFG"
STAGE="mcopy -o -i \$ROOT/disk.img $WORK/ATONE.ELF ::ATONE.ELF && mcopy -o -i \$ROOT/disk.img $WORK/ALOAD.ELF ::ALOAD.ELF && mcopy -o -i \$ROOT/disk.img $CFG ::WAVE.CFG"
ACCEL=""; [ -e /dev/kvm ] && ACCEL="-enable-kvm"
cd "$ROOT"
# HOSTSTALL="MS EVERY_S COUNT": whole-guest host stalls of this run's own QEMU
HSPID=""
if [ -n "${HOSTSTALL:-}" ]; then
  bash "$ROOT/tools/cp/audio2/hoststall.sh" "$WAV" "$ROOT/obj/t3_intel.log" $HOSTSTALL > "$OUT/$RUN.hoststall.txt" 2>&1 &
  HSPID=$!
fi
T0=$(date +%s)
CP_AUDIO_WAV="$WAV" T3_QEMU_ARGS="$ACCEL" T3_TIMEOUT="$((SECS + 240))" T3_STAGE="$STAGE" T3_MIN_LINES=1 \
  TOOLS_SKIP=1 bash tools/t3_tier.sh intel > "$OUT/$RUN.tier.log" 2>&1
T1=$(date +%s)
[ -n "$HSPID" ] && { kill "$HSPID" 2>/dev/null; wait "$HSPID" 2>/dev/null; }
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT/$RUN.tier.log" | tail -1)"
[ -r "$CONSOLE" ] && cp "$CONSOLE" "$OUT/$RUN.console.log"
S="$OUT/$RUN.summary.txt"
{
  echo "# run-atone $RUN: secs=$SECS atone='$AARGS' aload='$LARGS' host=$(hostname) wall=$((T1 - T0))s load=$(cut -d' ' -f1-3 /proc/loadavg)"
  grep -aoE '\[(AT|AL)\] .*' "$OUT/$RUN.console.log" | grep -v 'xrun-state' | tail -40
  echo "AT xrun-state rows: $(grep -ac '\[AT\] xrun-state' "$OUT/$RUN.console.log")"
  [ -n "${HOSTSTALL:-}" ] && { echo "host stalls (HOSTSTALL=$HOSTSTALL):"; cat "$OUT/$RUN.hoststall.txt"; }
  grep -aoE '\[CONS\] lines=.*' "$OUT/$RUN.console.log" | sed 's/ diag-off.*//' | tail -1
  grep -aoE '\[SND\] .*' "$OUT/$RUN.console.log" | grep -v '^\[SND\] xrun n=' | tail -40
  echo "SND xrun rows: $(grep -acE '\[SND\] xrun n=' "$OUT/$RUN.console.log")"
  grep -a -i 'rate control\|audio:' "$OUT/$RUN.console.log" | head -5
  if [ -f "$WAV" ]; then
    python3 "$ROOT/tools/cp/hunt/wavgaps.py" "$WAV"
    python3 "$ROOT/tools/cp/audio/wavcheck.py" "$WAV" --freq 440 --min-tone-s 1 || true
  else
    echo "no WAV"
  fi
} > "$S" 2>&1
rm -f "$WAV"
cat "$S"
