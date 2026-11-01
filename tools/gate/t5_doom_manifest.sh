#!/usr/bin/env bash
# t5_doom_manifest.sh — boot the intel guest with DOOM.BIN as the ONLY wave member
# and report what the guest does with it.
#
# Why this and not the desktop menu: the historical T5 failure was "DOOM never
# launched" — the desktop path is a launcher/input question, which is a different
# defect from "the binary cannot boot". A ::WAVE.CFG manifest (GB-072) removes the
# launcher from the question, so the two failures can be told apart instead of
# argued about.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${DOOM_BIN_PATH:-$ROOT/obj/intel/doom.bin}"
WAD="${DOOM_WAD_PATH:-$ROOT/doom1.wad}"
OUT="${DOOM_OUT:-/tmp/t5_doom_manifest.log}"
TIMEOUT_S="${DOOM_TIMEOUT:-1200}"

[ -f "$BIN" ] || { echo "FATAL: $BIN not built (make ARCH=intel obj/intel/doom.bin)"; exit 2; }
echo "=== doom bin: $(stat -c %s "$BIN") bytes"
[ -f "$WAD" ] && echo "=== iwad: $(stat -c %s "$WAD") bytes" || echo "=== note: no IWAD — DOOM will report a missing WAD and exit"

CFG="$ROOT/tests/fixtures/abi/wave_doom.cfg"
printf 'DOOM.BIN\n' > "$CFG"

STAGE="mcopy -o -i \"\$ROOT/disk.img\" \"$BIN\" ::DOOM.BIN"
if [ -f "$WAD" ]; then
  STAGE="$STAGE && mcopy -o -i \"\$ROOT/disk.img\" \"$WAD\" ::DOOM1.WAD"
fi
STAGE="$STAGE && mcopy -o -i \"\$ROOT/disk.img\" \"$CFG\" ::WAVE.CFG"

echo "=== booting intel guest (deadline ${TIMEOUT_S}s), log $OUT ==="
T3_TIMEOUT="$TIMEOUT_S" T3_STAGE="$STAGE" bash "$ROOT/tools/t3_tier.sh" intel > "$OUT" 2>&1
RC=$?
# GB-140: the guest console is T3_LOG, NOT the captured stdout (which holds the
# verdict summary).  Derive the console path from the machine contract, fail
# loudly if the tier did not print it — never grep $OUT for program markers.
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
if [ -z "$CONSOLE" ] || [ ! -r "$CONSOLE" ]; then
  echo "t5_doom_manifest: FAIL — t3_tier did not print a readable T3_LOG (GB-140 clause 3)"; exit 1
fi
echo "=== tier exit: $RC (console $CONSOLE)"
echo "=== verdict line:"
grep -aE "T3_intel: [0-9]+ console" "$OUT" | tail -1
echo "=== guest evidence:"
grep -aE "\[WAVE-TRY\] DOOM|\[ARRIVED\] DOOM|\[KERNEL\] pid=[0-9]+ \(DOOM|DOOM.*(IWAD|WAD|exit|fault)|memory fault|System halt|\[WAVE-CFG\]|manifest" "$CONSOLE" | tail -20
