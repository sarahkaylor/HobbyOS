#!/usr/bin/env bash
# tools/run_integration_gated.sh — GB-057: run the integration tier (MODE=test)
# and gate the result on printed FAIL lines.
#
# History: `make test` / `make test_intel` boot QEMU whose `-serial stdio`
# goes to QEMU's stdout; the wave prints `[...] FAIL` lines per failing check,
# then the kernel prints "System halt." and QEMU powers off with exit 0.  The
# QEMU exit code was therefore never a gate — 222 intel failures hid behind a
# green exit.  This runner redirects the serial capture to a log, bounds the
# boot by the project's deadlock rule, and runs tools/integration_gate.sh on
# the log so a printed FAIL fails the tier.
#
# Usage:
#   tools/run_integration_gated.sh [arm|intel] [--build] [--keep-image]
#     --build       build the disk image first (make ARCH MODE=test disk.img)
#     --keep-image  don't clean the disk image after the run
#   TIMEOUT=?       override the boot budget (default 600 s)
#   GATE_ALLOW='RE,RE'   quarantine regexes passed to integration_gate.sh
#
# Exit: 0 = tier passed (0 un-quarantined FAIL lines), 1 = tier failed or
# timed out / build failed.
set -u
cd "$(dirname "$0")/.." || exit 1

ARCH="${1:-intel}"
[ "$ARCH" = arm ] || [ "$ARCH" = intel ] || { echo "arch must be arm|intel" >&2; exit 2; }
BUILD=0; KEEP=0
for a in "$@"; do
  case "$a" in --build) BUILD=1;; --keep-image) KEEP=1;; esac
done

TIMEOUT="${TIMEOUT:-600}"
GATE_ALLOW="${GATE_ALLOW:-}"
LOG="gate_${ARCH}.log"
: > "$LOG"

if [ "$BUILD" = 1 ]; then
  echo "[gate] building $ARCH MODE=test disk..."
  if ! make "ARCH=$ARCH" MODE=test disk.img >"build_gate_$ARCH.log" 2>&1; then
    echo "[gate] build FAILED; see build_gate_$ARCH.log" >&2
    exit 1
  fi
fi

# Build --allow args from a comma/space-separated GATE_ALLOW list.
declare -a allow_args=()
if [ -n "$GATE_ALLOW" ]; then
  IFS=', ' read -r -a _re <<< "$GATE_ALLOW"
  for re in "${_re[@]}"; do [ -n "$re" ] && allow_args+=("--allow=$re"); done
fi

echo "[gate] booting $ARCH MODE=test (budget ${TIMEOUT}s), serial capture -> $LOG"
# QEMU_CMD's -serial stdio lands in make's stdout; redirect it to the log.
if ! timeout "$TIMEOUT" make "ARCH=$ARCH" MODE=test run >"$LOG" 2>&1; then
  echo "[gate] $ARCH boot timed out / failed after ${TIMEOUT}s (deadlock rule) -> TIER FAILED"
  tools/integration_gate.sh "${allow_args[@]}" --json "$LOG" 2>/dev/null || true
  exit 1
fi

echo "[gate] boot finished; gating the serial capture..."
tools/integration_gate.sh "${allow_args[@]}" "$LOG"
rc=$?
if [ "$KEEP" = 0 ]; then
  make fresh_disk >/dev/null 2>&1 || rm -f disk.img
fi
exit $rc
